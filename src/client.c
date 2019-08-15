#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <ctype.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "ranvar.h"
#include <limits.h>
#include "sys/sendfile.h"
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <netinet/tcp.h>
#include <pthread.h>
//#include <sys/fcntl.h> 
#include "client.h"
#include "common.h"


// command line arguments
int serverPort;
char config_name[80];
char tcp_congestion_name[80];
char distributionFile[80];
char logFile_name[80];
char logIteration_name[80];

// input parameters
int num_dest;
int num_persistent_servers;
int *dest_port;
char (*dest_addr)[20];

// Flow size generator 
EmpiricalRandomVariable *empRV;

// Sleep time generator (exponential)
ExponentialRandomVariable *expRV;

int num_fanouts;
int *fanout_size;
int *fanout_prob;
int fanout_prob_total;

int iter;
double load;
int period;

// per-iteration variables
int *iteration_fanout;
uint *iteration_findex;
int *iteration_file_size;
int *iteration_destination;
int *iteration_sleep_time;
struct timeval *start_time;
struct timeval *stop_time;
uint *dest_file_count;
uint64_t *backlogged_bytes;

// sockets for communicating with each destination
int *sockets;            

// threading
pthread_mutex_t inc_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_t *threads;
// Per-dest condition variables to wake up client threads for data transfer.
// Used only when data transfer is from the client->server.
pthread_mutex_t *wake_mutexes;
pthread_cond_t  *wake_conds;
int *dest_req_available;
int *dest_req_index;
int *dest_req_fsize;

int req_file_count;
int req_index;
FILE *fd_log;
FILE *fd_it;
        
int client_num;

// direction of data transmission
#define MAX_WRITE 104857600  // 100MB
int reverse_dir;
char flowbuf[MAX_WRITE];

// effectively single-threaded client
int single_threaded;
          
int main (int argc, char *argv[]) {

  // read command line arguments
  read_args(argc, argv);

  // initialize random seed
  if (client_num)
    srand(client_num);
  else {
    struct timeval time;
    gettimeofday(&time, NULL);
    srand((time.tv_sec*1000000) + time.tv_usec);
  }

  // read input configuration
  read_config();

  // open log files
  fd_log = fopen(logFile_name, "w");
  fd_it = fopen(logIteration_name, "w");

  // set up per-iteration variables
  set_iteration_variables();

  // open connections to destination address/port combos
  open_connections();

  // launch threads
  threads = launch_threads();
  printf("===\n");

  // run iterations
  run_iterations();

  // check for completion of all threads
  for (int i = 0; i < num_dest - num_persistent_servers; i++) {
    pthread_join(threads[i], NULL);
  }
  printf("All iterations completed. Processing statistics...\n");

  // process statistics
  process_stats();
 
  printf("client terminated...\n");

  // clean up; especially free allocated memory
  cleanup();

  return 0;
}

void run_iterations() {
  if (period < 0) {
    req_index = 0;
    run_iteration(req_index);
  } 
  else {
    struct timeval t_start;
    struct timeval t_end;
    int timediff_us;
    int overtime_us;
    overtime_us = 0;
    for (int i = 0; i < iter; i++) {
      timediff_us = iteration_sleep_time[i];
      if (i > 0) {
        timediff_us -= interval_us(t_start, t_end);
        if (timediff_us < 0) {
#ifdef DEBUG
          printf("Overtime past request!\n");
#endif
          overtime_us -= timediff_us;
        } else {
          usleep(timediff_us);
        }
      }
      else {
        usleep(timediff_us);
      }
      gettimeofday(&t_start, NULL);
      run_iteration(i);
      gettimeofday(&t_end, NULL);
    }
    float overtime_per_req_us = (float)overtime_us / (float)iter;
    printf("===\nOverall head-of-line waiting time: "
           "%d us (per request %f)\n===\n",
           overtime_us, overtime_per_req_us);
  }
}

void cleanup() {  
  free(dest_port);
  free(dest_addr);
  free(sockets);
  free(threads);
  free(dest_file_count);

  free(fanout_size);
  free(fanout_prob);

  free(iteration_fanout);
  free(iteration_file_size);
  free(iteration_destination);  
  free(iteration_sleep_time);
  free(stop_time);
  free(start_time);
  free(backlogged_bytes);

  free(wake_mutexes);
  free(wake_conds);
  free(dest_req_available);
  free(dest_req_index);
  free(dest_req_fsize);

  fclose(fd_log);
  fclose(fd_it);
}

void process_stats() {
  // stats for all iterations
  uint max_iter_usec = 0;
  uint min_iter_usec = UINT_MAX;
  uint avg_iter_usec = 0;
  
  // stats per fanout
  uint *f_max_iter_usec = (uint*)malloc(num_fanouts * sizeof(uint));
  uint *f_min_iter_usec = (uint*)malloc(num_fanouts * sizeof(uint));
  uint *f_avg_iter_usec = (uint*)malloc(num_fanouts * sizeof(uint));
  uint *f_iter_count = (uint*)malloc(num_fanouts * sizeof(uint));
  
  for (int i = 0; i < num_fanouts; i++) {
    f_max_iter_usec[i] = 0;
    f_min_iter_usec[i] = UINT_MAX;
    f_avg_iter_usec[i] = 0;
    f_iter_count[i] = 0;
  }
  
  // file stats
  uint max_file_usec = 0;
  uint min_file_usec = UINT_MAX;
  uint avg_file_usec = 0;
  uint file_count = 0;

  // overall stats
  struct timeval *overall_start = NULL;
  struct timeval *overall_stop = NULL;
  uint64_t overall_bytes = 0;

  printf("Request completion stats are only reported for non-backlogged transfers.\n");
  for (int i = num_persistent_servers; i < iter; i++) {
    struct timeval *i_start = NULL;
    struct timeval *i_stop = NULL;
    
    for (int j = 0; j < iteration_fanout[i]; j++) {
      uint index = i * num_dest + j;
      
      // find the start and stop time of the iteration
      if (j == 0) {
	i_start = &start_time[index];
	i_stop = &stop_time[index];
      }
      else {
	if (start_time[index].tv_sec < i_start->tv_sec || 
	    (start_time[index].tv_sec == i_start->tv_sec && start_time[index].tv_usec < i_start->tv_usec)) {
	  i_start = &start_time[index];
	}
	
	if (stop_time[index].tv_sec > i_stop->tv_sec || 
	    (stop_time[index].tv_sec == i_stop->tv_sec && stop_time[index].tv_usec > i_stop->tv_usec)) {
	  i_stop = &stop_time[index];
	}
      }
      
      // measure file transfer completion time
      int f_sec = stop_time[index].tv_sec - start_time[index].tv_sec;
      int f_usec = stop_time[index].tv_usec - start_time[index].tv_usec;      
      f_usec += (f_sec * 1000000);
      if (f_usec < 0) {
        printf("request index %d start: %ld s %ld us stop: %ld s %ld us\n",
               index, start_time[index].tv_sec, start_time[index].tv_usec,
               stop_time[index].tv_sec, stop_time[index].tv_usec);
      }
      assert(f_usec >= 0);
      
      if ((uint)f_usec > max_file_usec) 
	max_file_usec = f_usec;
      
      if ((uint)f_usec < min_file_usec)
	min_file_usec = f_usec;
      
      avg_file_usec += f_usec;
      file_count++;
      
      write_logFile("File",iteration_file_size[index], f_usec, start_time[index].tv_sec*1000+start_time[index].tv_usec/1000);

#ifdef DEBUG    
      printf("File: %d,%d, size: %u duration: %u usec\n", i, j, iteration_file_size[index], f_usec);
#endif
    }

    // update overall stats
    if (overall_start == NULL || 
	(i_start->tv_sec < overall_start->tv_sec || 
	 (i_start->tv_sec == overall_start->tv_sec && i_start->tv_usec < overall_start->tv_usec))) {
      overall_start = i_start;
    }
	
    if (overall_stop == NULL ||
	(i_stop->tv_sec > overall_stop->tv_sec || 
	 (i_stop->tv_sec == overall_stop->tv_sec && i_stop->tv_usec > overall_stop->tv_usec))) {
      overall_stop = i_stop;
    }
    overall_bytes += iteration_file_size[i*num_dest]*iteration_fanout[i];

    // measure iteration completion time
    int i_sec = i_stop->tv_sec - i_start->tv_sec;
    int i_usec = i_stop->tv_usec - i_start->tv_usec;
    i_usec += (i_sec*1000000);
    assert(i_usec >= 0);
        
    if ((uint)i_usec > max_iter_usec)
      max_iter_usec = i_usec;
    
    if ((uint)i_usec < min_iter_usec)
      min_iter_usec = i_usec;
    
    avg_iter_usec += i_usec;
    
    // measure iteration completion time per fanout
    int f_index = 0;
    for (int k = 0; k < num_fanouts; k++) {
      if (fanout_size[k] == iteration_fanout[i]) {
	f_index = k;
	break;
      }
    }
    
    if ((uint)i_usec > f_max_iter_usec[f_index]) 
      f_max_iter_usec[f_index] = i_usec;
    
    if ((uint)i_usec < f_min_iter_usec[f_index]) 
      f_min_iter_usec[f_index] = i_usec;
       
    f_avg_iter_usec[f_index] += i_usec;
    f_iter_count[f_index]++;

    write_logFile("Iteration", iteration_file_size[i*num_dest]*iteration_fanout[i], i_usec, i_start->tv_sec*1000+i_start->tv_usec/1000);

#ifdef DEBUG    
    printf("Iteration: %d, total size: %u duration: %u usec\n", i, iteration_file_size[i*num_dest]*iteration_fanout[i], i_usec);
#endif

  }
  
  // measure overall throuhgput
  int64_t total_sec = overall_stop->tv_sec - overall_start->tv_sec;
  int64_t total_usec = overall_stop->tv_usec - overall_start->tv_usec;
  total_usec += (1000000*total_sec);

  printf("===\n");
  printf("Total RX Throughput: %fMbps\n", overall_bytes*8.0/total_usec);
  
  printf("=== Stats for fanout sizes ===\n");  
  for (int i = 0 ; i < num_fanouts; i++) {
    printf("Fanout: %d - count: %d\n", fanout_size[i], f_iter_count[i]);
    if (f_iter_count[i] > 0) {
      printf("Max duration : %u usec\n", f_max_iter_usec[i]);
      printf("Avg iteration: %u usec\n", f_avg_iter_usec[i] / f_iter_count[i]);
      printf("Min duration:  %u usec\n", f_min_iter_usec[i]);
    }
  }
  
  
  printf("=== Stats for flows ===\n");  
  printf("Max duration : %u usec\n", max_file_usec);
  printf("Avg iteration: %u usec\n", avg_file_usec / file_count);
  printf("Min duration: %u usec\n", min_file_usec);
  
  printf("=== Stats for requests ===\n");
  printf("Max iteration: %u usec\n", max_iter_usec);
  printf("Avg iteration: %u usec\n", avg_iter_usec / iter);
  printf("Min iteration: %u usec\n", min_iter_usec);

  printf("===\n");

  float avg_tput = 0;
  for (int i = 0; i < num_persistent_servers; i++) {
    printf("Persistent thread %d transferred approx %llu bytes\n", i, backlogged_bytes[i]);
    avg_tput += (float)backlogged_bytes[i]*8.0/total_usec;
  }
  printf("total experiment time: %lld us\n", total_usec);
  avg_tput /= num_persistent_servers;
  printf("Average throughput for backlogged requests: %fMbps\n", avg_tput);
}

pthread_t *launch_threads() {
  pthread_t *threads = (pthread_t*)malloc(num_dest * sizeof(pthread_t));
  wake_mutexes = (pthread_mutex_t*)malloc(num_dest * sizeof(pthread_mutex_t));
  wake_conds   = (pthread_cond_t* )malloc(num_dest * sizeof(pthread_cond_t));
  dest_req_available = (int*)malloc(num_dest * sizeof(int));
  dest_req_index     = (int*)malloc(num_dest * sizeof(int));
  dest_req_fsize     = (int*)malloc(num_dest * sizeof(int));
  
  for (int i = 0; i < num_dest; i++) {
    wake_mutexes[i] = PTHREAD_MUTEX_INITIALIZER;
    wake_conds[i]   = PTHREAD_COND_INITIALIZER;
    dest_req_available[i] = 0;
    dest_req_index[i]     = -1;
    dest_req_fsize[i]     = -1;
    int *l_index = (int*)malloc(sizeof(int));
    *l_index = i;
    if ((! reverse_dir) || (! single_threaded))
      pthread_create(&threads[i], NULL, listen_connection, l_index);
  }

  return threads;
}

void open_connections() {
  int sock_opt = 1;
  int ret;

  printf("Connecting to servers...\n");
  
  for (int i = 0; i < num_dest; i++) {
    struct sockaddr_in servaddr;
    int sock;
    sock = socket(AF_INET, SOCK_STREAM, 0);
    
    // set socket options
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &sock_opt, sizeof(sock_opt));
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &sock_opt, sizeof(sock_opt));
    if (setsockopt(sock, IPPROTO_TCP, TCP_CONGESTION, tcp_congestion_name, 80)<0) {
      printf("Unable to set TCP_CONGESTION option\n");
      exit(EXIT_FAILURE);
    }

    // connect to destination server (address/port)
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(dest_port[i]);
    inet_pton(AF_INET, dest_addr[i], &servaddr.sin_addr);
    
    ret = connect(sock, (struct sockaddr *) &servaddr, sizeof(servaddr));
    
    if (ret < 0) {
      printf("Unable to connect. error: %s\n", strerror(errno));
      sockets[i] = -1;
      exit(EXIT_FAILURE);
    }
    else {
      printf("Connected to %s on port %d\n", dest_addr[i], dest_port[i]);
      sockets[i] = sock;
    }
  }
}

void set_iteration_variables() {
  // generate fanout and files sizes for each iteration
  iteration_fanout = (int*)malloc(iter * sizeof(int));
  iteration_file_size = (int*)malloc(iter * num_dest * sizeof(int));
  iteration_destination = (int*)malloc(iter * num_dest * sizeof(int));
  iteration_sleep_time = (int*)malloc(iter * sizeof(int));
  stop_time = (struct timeval*)malloc(iter * num_dest * sizeof(struct timeval));
  start_time = (struct timeval*)malloc(iter * num_dest * sizeof(struct timeval));
  backlogged_bytes = (uint64_t*)malloc(num_persistent_servers * sizeof(uint64_t));

  int *temp_list = (int*)malloc(num_dest * sizeof(int));
  int *dest_list = (int*)malloc(num_dest * sizeof(int));
  
  for (int i = 0; i < num_dest; i++) {
    temp_list[i] = i;
  }

  /* Schedule persistent transfers */
  for (int i = 0; i < num_persistent_servers; i++) {
    iteration_fanout[i] = 1;
    iteration_sleep_time[i] = 1;
    /* signal backlogged transfer to the server  */
    iteration_file_size[i * num_dest] = 0;
    /* Transfer to the i'th persistent server */
    int dest = num_dest - num_persistent_servers + i;
    iteration_destination[i * num_dest] = dest;
    dest_file_count[dest]++;
  }

  /* Schedule small requests */
  for (int i = num_persistent_servers; i < iter; i++) {
    // generate fanout size
    int val = rand() % fanout_prob_total;
    for (int j = 0; j < num_fanouts; j++) {
      if (val < fanout_prob[j]) {
	iteration_fanout[i] = fanout_size[j];
	break;
      }
      else {
	val -= fanout_prob[j];
      }
    }

#ifdef DEBUG    
    printf("%d - Fanout size: %d\n", i, iteration_fanout[i]);
#endif

    // generate sleep times
    int sltime = (int)expRV->value();
    if (sltime < 0)
      sltime = INT_MAX;
    if (sltime == 0)
      sltime = 1;
    iteration_sleep_time[i] = sltime;

    memcpy(dest_list, temp_list, num_dest * sizeof(int));
    int dest_count = num_dest - num_persistent_servers;
    
    // generate file sizes and destination
    uint64_t reqSize = empRV->value();

    for (int j = 0; j < iteration_fanout[i]; j++) {
      // file size
      iteration_file_size[i * num_dest + j]  = (reqSize/iteration_fanout[i]);  
      // destination
      val = rand() % dest_count;
      iteration_destination[i * num_dest + j] = dest_list[val];
      dest_file_count[dest_list[val]]++;
      dest_count--;
      dest_list[val] = dest_list[dest_count];

#ifdef DEBUG      
      printf("%d, %d - Dest: %d, File: %d\n", i, j, iteration_destination[i * num_dest +j], iteration_file_size[i * num_dest + j]);
#endif

    }
  }

  for (int i = 0; i < num_dest; i++) 
    printf("Server[%d] file count: %d\n", i, dest_file_count[i]);

  free(temp_list);
  free(dest_list);
}

void read_args(int argc, char*argv[]) {
  // default values
  int config_given = 0;

  strcpy(logFile_name, "log");
  strcpy(logIteration_name, "log");
  strcpy(tcp_congestion_name, "reno");

  client_num = 0;
  reverse_dir = 0;
  single_threaded = 0;

  int i = 1;
  while (i < argc) {
    if (strcmp(argv[i], "-c") == 0) {
      strcpy(config_name, argv[i+1]);
      config_given = 1;
      i += 2;
    } else if (strcmp(argv[i], "-l") == 0) {
      strcpy(logFile_name,argv[i+1]);
      strcpy(logIteration_name, argv[i+1]);
      i +=2;
    } else if (strcmp(argv[i], "-s") == 0) {
      client_num = atoi(argv[i+1]);
      i+=2;
    } else if (strcmp(argv[i], "-h") == 0) {
      print_usage();
      exit(EXIT_FAILURE);
    } else if (strcmp(argv[i], "-r") == 0) {
      reverse_dir = 1;
      i += 1;
    } else if (strcmp(argv[i], "-t") == 0) {
      strcpy(tcp_congestion_name, argv[i+1]);
      i += 2;
    } else if (strcmp(argv[i], "-a") == 0) {
      single_threaded = 1;
      i += 1;
    } else {
      printf("invalid option: %s\n", argv[i]);
      print_usage();
      exit(EXIT_FAILURE);
    }
  }

  if (!config_given) {
    printf("no configuration file provided.\n");
    print_usage();
    exit(EXIT_FAILURE);
  }

  strcat(logFile_name,"_flows.out");
  strcat(logIteration_name,"_reqs.out");
  printf("Random seed: %d\n", client_num);
}

void print_usage() {
  printf("usage: client [options]\n");
  printf("options:\n");
  printf("-c <string>                  configuration file\n");
  printf("-l <string>                  prefix for output log files\n");
  printf("-s <integer>                 seed value\n");
  printf("-r                           transfer data client->server (default server->client)\n");
  printf("-t <string>                  tcp congestion control algorithm (default reno)\n");
  printf("-a                           transfer data from client->server using a single thread\n");
  printf("-h                           display usage information and quit\n");
}

void write_logFile(const char *type,int  size, int duration, long long start_time){
  if (strcmp(type,"File") == 0){
    fprintf(fd_log, "Size:%u, Duration(usec):%u, StartTime(ms):%llu\n",size,duration, start_time);
  }
  if (strcmp(type,"Iteration") == 0){
    fprintf(fd_it, "Size:%u, Duration(usec):%u, StartTime(ms):%llu\n",size,duration, start_time);
  }            
}

void read_config() {
  FILE *fd;
  char line[256];

  printf("Reading configuration file: %s\n", config_name);
 
  // first pass: sanity check
  fd = fopen(config_name, "r");
  int num_servers = 0;
  int num_fsize_dist = 0;
  int num_load = 0;
  int num_it = 0;
  num_persistent_servers = 0;
  num_fanouts = 0;
  while (fgets(line, 256, fd)) 
  {
    char key[80];
    sscanf(line, "%s", key);
    if (!strcmp(key, "server"))
      num_servers++;
    else if (!strcmp(key, "persistent_servers"))
      num_persistent_servers++;
    else if (!strcmp(key, "fanout"))
      num_fanouts++;
    else if (!strcmp(key, "req_size_dist")) {
      num_fsize_dist++;
      if (num_fsize_dist > 1) {
	fprintf(stderr, "config file formatting error: more than one req_size_dist\n");
	exit(EXIT_FAILURE);
      }
    } else if (!strcmp(key, "load")) {
      num_load++;
      if (num_load > 1) {
	fprintf(stderr, "config file formatting error: more than one load\n");
	exit(EXIT_FAILURE);
      }
    } else if (!strcmp(key, "num_reqs")) {
      num_it++;
      if (num_it > 1) {
	fprintf(stderr, "config file formatting error: more than one num_reqs\n");
	exit(EXIT_FAILURE);
      }
    } else {
      fprintf(stderr, "invalid key: %s\n", key);
      exit(EXIT_FAILURE);
    }
  }
  fclose(fd);
  if (num_servers < 1) {
    fprintf(stderr, "config file formatting error: must provide at least one server\n");
    exit(EXIT_FAILURE);
  }
  if (num_fsize_dist < 1) {
    fprintf(stderr, "config file formatting error: missing req_size_dist\n");
    exit(EXIT_FAILURE);
  }
  if (num_fanouts < 1) {
    fprintf(stderr, "config file formatting error: must provide at least one fanout\n");
    exit(EXIT_FAILURE);
  }
  if (num_load < 1) {
    fprintf(stderr, "config file formatting error: missing load\n");
    exit(EXIT_FAILURE);
  }
  if (num_it < 1) {
    fprintf(stderr, "config file formatting error: missing num_reqs\n");
    exit(EXIT_FAILURE);
  }
  if (num_persistent_servers >= num_servers) {
    fprintf(stderr, "config file error: # persistent servers must be smaller"
            " than the total # servers!\n");
    exit(EXIT_FAILURE);
  }

  // initialize
  printf("===\nNumber of servers: %d\n", num_servers);
  num_dest = num_servers;
  dest_addr = (char (*)[20])malloc(num_dest * sizeof(char[20]));
  dest_port = (int*)malloc(num_dest * sizeof(int));
  sockets = (int*)malloc(num_dest * sizeof(int));
  dest_file_count = (uint*)malloc(num_dest * sizeof(uint));

  printf("Number of fanouts: %d\n", num_fanouts);
  fanout_size = (int*)malloc(num_fanouts * sizeof(int));
  fanout_prob = (int*)malloc(num_fanouts * sizeof(int));
  fanout_prob_total = 0;

  printf("Number of backlogged servers: %d\n===\n", num_persistent_servers);

  if (reverse_dir)
    printf("Transmitting data from client to server\n");
  else
    printf("Transmitting data from server to client\n");

  // second pass: parse
  fd = fopen(config_name, "r");
  num_servers = 0;
  num_fanouts = 0;
  while (fgets(line, 256, fd)) 
  {
    char key[80];
    sscanf(line, "%s", key);
    if (!strcmp(key, "server")) {
      sscanf(line, "%s %s %d\n", key, dest_addr[num_servers], &dest_port[num_servers]);
      dest_file_count[num_servers] = 0;
      printf("Server[%d]: %s, Port:%d\n", num_servers, dest_addr[num_servers], dest_port[num_servers]);
      num_servers++;
    }

    if (!strcmp(key, "persistent_servers")) {
      sscanf(line, "%s %d\n", key, &num_persistent_servers);
      printf("# persistent servers: %d\n", num_persistent_servers);
    }

    if (!strcmp(key, "fanout")) {
      sscanf(line, "%s %d %d\n", key, &fanout_size[num_fanouts], &fanout_prob[num_fanouts]);
      fanout_prob_total += fanout_prob[num_fanouts];
      printf("Fanout: %d, Prob: %d\n", fanout_size[num_fanouts], fanout_prob[num_fanouts]);
      num_fanouts++;
    }

    if (!strcmp(key, "req_size_dist")) {
      sscanf(line, "%s %s\n", key, distributionFile);
      empRV = new EmpiricalRandomVariable(INTER_INTEGRAL, client_num*13);
      empRV->loadCDF(distributionFile);
      printf("Loading request size distribution: %s\n", distributionFile);
      printf("Avg request size: %.2f bytes\n", empRV->avg());
    }

    if (!strcmp(key, "load")) {
      sscanf(line, "%s %lfMbps\n", key, &load);
      printf("load: %.2f Mbps\n", load);
    }

    if (!strcmp(key, "num_reqs")) {
      sscanf(line, "%s %d\n", key, &iter);
      printf("Number of Requests: %d\n", iter);
    }
  }
  fclose(fd);

  if (load > 0) {
    period = 8 * empRV->avg() / load;
    if (period <= 0) {
      printf("period not positive: %d\n", period);
      exit(EXIT_FAILURE);
    }
  } else {
    period = -1;
  }
  printf("Average flow inter-arrival period: %dus\n===\n", period);
  expRV = new ExponentialRandomVariable(period, client_num*133);
  /* If there are persistent connections, add that to the # requests. */
  iter += num_persistent_servers;
}

// Helper for writing index and file size into socket
void write_sock_index_size(int sock, uint index, uint size) {
  char buf[30];
  memcpy(buf, &index, sizeof(uint));
  memcpy(buf + sizeof(uint), &size, sizeof(uint));
  int n = write(sock, buf, 2 * sizeof(uint));
  if (n < 0) {
    printf("error in request write: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }
}

// Helper for reading index and file size from socket
void read_sock_index_size(int sock, uint *index, uint *size) {
  int total;
  int meta_read_size = 2 * sizeof(uint);
  int n;
  char buf[READBUF_SIZE];

  total = 0;
  while (total < meta_read_size) {
    char readbuf[50];

    n = read(sock, readbuf, meta_read_size - total);
    if (n < 0) {
      printf("error in meta-data read: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
    }

    memcpy(buf + total, readbuf, n);
    total += n;
  }
  if (total != meta_read_size) {
    printf("Read meta data size is incorrect!\n");
    exit(EXIT_FAILURE);
  }
    
  memcpy(index, buf, sizeof(int));
  memcpy(size, buf + sizeof(int), sizeof(int));
}

void run_iteration(int it) {
   
  if (period < 0) {
    req_file_count = iteration_fanout[it];
  }

#ifdef DEBUG
  struct timeval tstart, tstop;
  int usec;
  int sec;
  printf("Iteration: %d, fanout: %d\n", it, iteration_fanout[it]);
  gettimeofday(&tstart, NULL);
#endif

  // send requests
  for (int j = 0; j < iteration_fanout[it]; j++) {
    //printf("Iteration: %d, Reqeust: %d..\n", i, j);
    uint index = it * num_dest + j;

    if (! reverse_dir) {
      // Server will echo written metadata to the listening thread, unblocking it.
      gettimeofday(&start_time[index], NULL);
      write_sock_index_size(sockets[iteration_destination[index]],
                              index,
                              iteration_file_size[index]);

    } else {
      // Unblock the listening thread directly since a request must be sent now
      int dest = iteration_destination[index];
      uint f_size = iteration_file_size[index];
      uint total = f_size;
      if (single_threaded) {
        int sock = sockets[iteration_destination[index]];
        gettimeofday(&start_time[index], NULL);
        write_sock_index_size(sock, index, f_size);
        /* Send f_size bytes */
        if (write_exact(sock, flowbuf, total, MAX_WRITE, true)
            != f_size) {
          printf("failed to write: %d\n", total);
          exit(EXIT_FAILURE);
        }
        // Read from server to ensure size is echoed correctly
        uint check_f_index;
        uint check_f_size;
        read_sock_index_size(sock, &check_f_index, &check_f_size);
        gettimeofday(&stop_time[index], NULL);
        assert(f_size  == check_f_size);
        assert(index == check_f_index);
        if (it % 1000 == 0 && it > 0)
          printf("Sent %d files from single-threaded client\n", it);
      } else {
        pthread_mutex_lock(&wake_mutexes[dest]);
        while (dest_req_available[dest] != 0) {
          printf("Head-of-line blocking :(\n");
          printf("Server %d index %d size %d\n", dest, index, f_size);
          pthread_cond_wait(&wake_conds[dest], &wake_mutexes[dest]);
        }
        gettimeofday(&start_time[index], NULL);
        dest_req_available[dest]++;
        dest_req_index[dest] = index;
        dest_req_fsize[dest] = f_size;
        pthread_cond_signal(&wake_conds[dest]);
        pthread_mutex_unlock(&wake_mutexes[dest]);
      }
    }
  }

#ifdef DEBUG    
  gettimeofday(&tstop, NULL);
  sec = tstop.tv_sec - tstart.tv_sec;
  usec = tstop.tv_usec - tstart.tv_usec;  
  if (usec < 0) {
    usec += 1000000;
    sec--;
  } 
  usec += sec * 1000000;
  printf("Duration: %u usec\n", usec); 
#endif
  
}

void *listen_connection(void *ptr) {
  int index = *((int *)ptr);
  free(ptr);

  int sock = sockets[index];
  char buf[READBUF_SIZE];
  uint file_count = 0;
  int n;

  uint f_index;
  uint f_size;

  while(file_count < dest_file_count[index]) {
    //printf("%d - wait for meta data, left: %d\n", index, (dest_file_count[index] - file_count));
    int total = 0;

    if (!reverse_dir) {
      read_sock_index_size(sock, &f_index, &f_size);
    } else { // data transfer direction client->server
      pthread_mutex_lock(&wake_mutexes[index]);
      while(dest_req_available[index] == 0) {
        pthread_cond_wait(&wake_conds[index], &wake_mutexes[index]);
      }
      // There is an available request to process
      f_index = dest_req_index[index];
      f_size  = dest_req_fsize[index];
      // Decrement req_available here to allow another request to queue up
      dest_req_available[index] --;
      pthread_cond_signal(&wake_conds[index]);
      pthread_mutex_unlock(&wake_mutexes[index]);
    }

    total = f_size;

    if (! reverse_dir) {
      /* Read f_size bytes */
      if (total > 0) {
        do {
          int readsize = total;
          if (readsize > READBUF_SIZE)
            readsize = READBUF_SIZE;

          n = read(sock, buf, readsize);
          if (n < 0) {
            printf("failed to read: %d %d\n", n, total);
            exit(EXIT_FAILURE);
          }

          total -= n;

        } while (total > 0 && n > 0);

      if (total > 0) {
        printf("failed to read: %d\n", total);
        exit(EXIT_FAILURE);
      }
#ifdef DEBUG
      printf("Received %d bytes from server\n", f_size);
#endif
      /* Read finished. */
      } else {
        /* Read indefinitely */
        n = 0;
        int b_index = index - num_dest + num_persistent_servers;
        backlogged_bytes[b_index] = 0;
        do {
          backlogged_bytes[b_index] += n;
          n = read(sock, buf, READBUF_SIZE);
        } while (n > 0);
        if (n < 0) {
          printf("Error in reading indefinitely!\n");
          exit(EXIT_FAILURE);
        }
      }
    } else {
      /* In the reverse direction, writes are serialized to avoid an extra
         round-trip time for full data transfer. */
      /* Write metadata first */
      if (! single_threaded) {
        write_sock_index_size(sock, f_index, f_size);
        /* Send f_size bytes */
        if (write_exact(sock, flowbuf, total, MAX_WRITE, true)
            != f_size) {
          printf("failed to write: %d\n", total);
          exit(EXIT_FAILURE);
        }
#ifdef DEBUG
        else {
          printf("Wrote %d bytes to server\n", total);
        }
#endif
        // Read from server to ensure size is echoed correctly
        uint check_f_index;
        uint check_f_size;
        read_sock_index_size(sock, &check_f_index, &check_f_size);
        assert(f_size  == check_f_size);
        assert(f_index == check_f_index);
      }
    }
    gettimeofday(&stop_time[f_index], NULL);

    file_count++;

    if (file_count % 1000 == 0)
      printf("Received %d files from Server[%d]\n", file_count, index);

    if (period < 0) {
      pthread_mutex_lock(&inc_lock);
      req_file_count--;
      if (req_file_count == 0) {
        req_index++;
        if (req_index < iter) {
          run_iteration(req_index);
        }
      }
      pthread_mutex_unlock(&inc_lock);
    }
  }

  printf("%d - All files received from destination, total: %d\n", index, file_count);

  close(sock);

  return 0;
}
