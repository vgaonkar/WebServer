//csuc_http_vrgaonkar.c
//Gaonkar, Vijay
//vrgaonkar
//Referred stackoverflow.com for calculating time used in printing server statistics

#include <stdio.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include "http_server.h"

#define FILE_BUF_SIZE       1024
#define TIME_BUF_SIZE       50
#define CONTENT_TYPE_SIZE   10
#define CONTENT_LENGTH_SIZE 10
#define LISTEN_BACKLOG      50

struct timespec upstart={0,0}, upend={0,0};
struct timespec request_time_start={0,0}, request_time_end={0,0};

enum 
{
  RUNNING,
  SHUTDOWN
};


typedef enum
{
  SERIALIZED,
  THREADED,
  FORKED,
  THREAD_POOL
}http_connection_strategy_t;


typedef enum
{
  ERROR,
  WARNING,
  INFO,
  DEBUG,
  DATA
}log_level_t;


static char* document_root                     = ".";
static int   port_num                          = 9000;
volatile sig_atomic_t status                   = RUNNING;
static log_level_t global_log_level            = WARNING;
static pthread_mutex_t job_queue_mutex         = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t total_requests_mutex    = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t requests_served_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t data_transferred_mutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_queue_not_empty_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t job_queue_not_full_cond  = PTHREAD_COND_INITIALIZER;
static int max_job_queue_size                  = 16;
static int cur_job_queue_size                  = 0;
/*static int a;
  static int b;
  static int c;*/
static int total_requests_served               = 0;
static int total_data_transferred              = 0;
static float total_service_request_time        = 0.0;
//static int d;
//static int e;
//static int f;
static int first_index;
static int cur_index;
static int max_workers;
static int* job_queue;
static http_connection_strategy_t strategy;


void print_log(log_level_t log_level, const char* message, ...)
{
  va_list args;
  va_start (args, message);

  switch (global_log_level)
  {
    case ERROR:
      if ((log_level == ERROR) || (log_level == DATA))
        vfprintf(stderr, message, args);
      break;

    case WARNING:
      if ((log_level == ERROR) || (log_level == WARNING) || (log_level == DATA))
        vfprintf(stderr, message, args);
      break;

    case INFO:
      if ((log_level == ERROR) || (log_level == WARNING) || (log_level == INFO) || (log_level == DATA))
        vfprintf(stderr, message, args);
      break;

    case DEBUG:
      if ((log_level == ERROR) || (log_level == WARNING) || (log_level == INFO) || (log_level == DEBUG) || (log_level == DATA))
        vfprintf(stderr, message, args);
      break;
  }

  va_end (args);
}


int next_request(int fd, http_request_t *request)
{
  char input[MAX_HEADER_LINE_LENGTH];
  char method_type[MAX_HEADER_NAME_LENGTH];
  FILE *input_fp;

  pthread_mutex_lock(&requests_served_mutex);
  clock_gettime(CLOCK_MONOTONIC, &request_time_start);
  pthread_mutex_unlock(&requests_served_mutex);

  input_fp = fdopen(fd, "r");
  if (input_fp == NULL)
    print_log(ERROR, "\n Failed to open the file descriptor for reading \n");

  request->header_count = 0;

  if (input_fp)
    fgets(input, MAX_HEADER_LINE_LENGTH, input_fp); 

  sscanf(input, "%s %s HTTP/%d.%d%*s", method_type, request->uri, &request->major_version, &request->minor_version);

  if(strcmp(method_type, "GET") == 0)
    request->method = HTTP_METHOD_GET;

  while (request->header_count < MAX_HEADERS)
  {
    fgets(input, MAX_HEADER_LINE_LENGTH, input_fp);
    if ((input[0] != '\n') && strstr(input, ":"))
    { 
      sscanf(input, "%s:%s", request->headers[request->header_count].field_name,
          request->headers[request->header_count].field_value);
      request->header_count++;
    }
    else
      break;
  }
  return 0;
}

void add_response_header(http_response_t *response, char field_name[MAX_HEADER_NAME_LENGTH], 
    char field_value[MAX_HEADER_VALUE_LENGTH])
{
  strcpy(response->headers[response->header_count].field_value, field_value);
  response->header_count++;
}

void handle_errors(http_response_t *response)
{
  int i=0;
  struct stat st;
  char   content_length[CONTENT_LENGTH_SIZE];
  char temp[URI_MAX];

  strcpy(temp,document_root);
  strcat(temp,"/404.html");
  if(lstat(temp,&st)==-1)
  {
    strcpy(temp,"\0");
    strcpy(temp,document_root);
    strcat(temp,"/400.html");
    if(lstat(temp,&st)==-1)
    {
      lstat("errors.html",&st);
      sprintf(content_length, "%lld", st.st_size);
      strcpy(response->resource_path,"errors.html");
      add_response_header(response, "Content-Type", "text/html");
      add_response_header(response, "Content-Length", content_length);
    }
    else //400
    {
      sprintf(content_length, "%lld", st.st_size);
      strcpy(response->resource_path,temp);
      add_response_header(response, "Content-Type", "text/html");
      add_response_header(response, "Content-Length", content_length);
    }

  }
  else //404
  {
    sprintf(content_length, "%lld", st.st_size);
    strcpy(response->resource_path,temp);
    add_response_header(response, "Content-Type", "text/html");
    add_response_header(response, "Content-Length", content_length);
  }
}


int build_response(const http_request_t *request, http_response_t *response)
{
  FILE   *file;
  struct stat st;
  struct tm *gm_time;
  time_t current_time;
  char   time_buf[TIME_BUF_SIZE];
  char   content_type[CONTENT_TYPE_SIZE];
  char   content_length[CONTENT_LENGTH_SIZE];
  response->header_count = 0;

  strcat(response->resource_path, request->uri);
  if (strstr(response->resource_path, "?"))
    sscanf(response->resource_path, "%[^?]", response->resource_path);

  if (strstr(response->resource_path, "#"))
    sscanf(response->resource_path, "%[^#]", response->resource_path);

  lstat(response->resource_path, &st);

  if(S_ISDIR(st.st_mode))
    strcat(response->resource_path, "index.html");

  lstat(response->resource_path, &st);

  current_time = time(NULL); //get the current time
  gm_time      = gmtime(&current_time); //convert current_time to local time representation
  strftime(time_buf, TIME_BUF_SIZE, "%a, %d %b %Y %H:%M:%S %Z",gm_time);

  char *ch = strrchr(response->resource_path, '.');
  if (ch == NULL)
    strcpy(content_type, "NO EXTENSION");
  else
    strcpy(content_type, ch);

  if ((strcmp(content_type, ".html") == 0))
    strcpy(content_type, "text/html");

  else if ((strcmp(content_type, ".txt") == 0))
    strcpy(content_type, "text");

  else if ((strcmp(content_type, ".css") == 0))
    strcpy(content_type, "text/css");

  else if ((strcmp(content_type, ".jpg") == 0))
    strcpy(content_type, "image/jpg");

  else if ((strcmp(content_type, ".jpeg") == 0))
    strcpy(content_type, "image/jpeg");

  else if ((strcmp(content_type, ".png") == 0))
    strcpy(content_type, "image/png");

  else if ((strcmp(content_type, ".js") == 0))
    strcpy(content_type, "application/javascript");

  else if ((strcmp(content_type, ".xml") == 0))
    strcpy(content_type, "application/xml");

  else if ((strcmp(content_type, ".mp3") == 0))
    strcpy(content_type, "audio/mpeg");

  else if ((strcmp(content_type, ".mpeg") == 0))
    strcpy(content_type, "video/mpeg");

  else if ((strcmp(content_type, ".mpg") == 0))
    strcpy(content_type, "video/mpeg");

  else if ((strcmp(content_type, ".mp4") == 0))
    strcpy(content_type, "video/mp4");

  else if ((strcmp(content_type, ".mov") == 0))
    strcpy(content_type, "video/quicktime");

  else  
    strcpy(content_type, "text/plain");

  response->major_version = request->major_version;
  response->minor_version = request->minor_version;

  add_response_header(response, "Date", time_buf);
  add_response_header(response, "Server", "CSUC HTTP");

  file = fopen(response->resource_path, "r");

  if (request->method == HTTP_METHOD_GET)
  {
    if (file) 
    {
      response->status = HTTP_STATUS_LOOKUP[HTTP_STATUS_OK];
      sprintf(content_length, "%lld", st.st_size);
      pthread_mutex_lock(&data_transferred_mutex);
      total_data_transferred += atoi(content_length);
      pthread_mutex_unlock(&data_transferred_mutex);
      add_response_header(response, "Content-Length", content_length);
      add_response_header(response, "Content-Type", content_type);
      fclose(file);
    }
    else
    {
      response->status = HTTP_STATUS_LOOKUP[HTTP_STATUS_NOT_FOUND];
      handle_errors(response);
    }
  }
  else
    response->status = HTTP_STATUS_LOOKUP[HTTP_STATUS_NOT_IMPLEMENTED];

  return 0;
}


int send_response(int fd, const http_response_t *response)
{
  FILE          *output_fp;
  FILE          *payload;
  char          file_buffer[FILE_BUF_SIZE];
  size_t        file_size;
  unsigned char line;
  int           i;

  output_fp = fdopen(fd, "w");
  if (output_fp == NULL)
    print_log(ERROR, "\n Failed to open the file descriptor for writing \n");
  fprintf(output_fp, "HTTP/%d.%d", response->major_version, response->minor_version);
  fprintf(output_fp, " %d %s\r\n",response->status.code,response->status.reason);

  for (i = 0; i < response->header_count; i++)
    fprintf(output_fp, "%s:%s\r\n", response->headers[i].field_name, response->headers[i].field_value);

  fprintf(output_fp, "\r\n");
  payload = fopen(response->resource_path, "r"); 
  print_log(DEBUG, "\n Sending response %s\n", response->resource_path);
  if(payload)
  { 
    while ((file_size = fread(file_buffer, 1, sizeof(file_buffer), payload)) > 0)
      fwrite(file_buffer, 1, file_size, output_fp);

    fclose(payload);
  }
  fclose(output_fp);

  pthread_mutex_lock(&total_requests_mutex);
  total_requests_served++;
  pthread_mutex_unlock(&total_requests_mutex);

  pthread_mutex_lock(&requests_served_mutex);
  clock_gettime(CLOCK_MONOTONIC, &request_time_end);
  total_service_request_time += ((double)request_time_end.tv_sec + 1.0e-9*request_time_end.tv_nsec) - 
    ((double)request_time_start.tv_sec + 1.0e-9*request_time_start.tv_nsec);
  pthread_mutex_unlock(&requests_served_mutex);

  return 0;
}


void handle_request(int fd)
{
  http_request_t  *request  = (http_request_t*) malloc(sizeof(http_request_t));
  http_response_t *response = (http_response_t*)malloc(sizeof(http_response_t));

  strcpy(response->resource_path, document_root);
  next_request(fd, request);
  build_response(request, response);
  send_response(fd, response);

  free(request);
  free(response);
}


void* thread_main(void *arg)
{
  int fd = *((int*)arg);
  handle_request(fd);
  close(fd);
  free((int*)arg);
  return (void*) 0;
}


void dispatch_connection_serialized(int fd)
{
  handle_request(fd);
  close(fd);
}


void dispatch_connection_threaded(int connect_fd)
{
  pthread_t thread_id;
  int       success;
  void      *return_value;
  int *fd = malloc(sizeof(int));
  *fd     = connect_fd;

  success = pthread_create(&thread_id, NULL, thread_main, fd);
  pthread_detach(thread_id);
  if (success != 0)
    print_log(ERROR, "\n Failed to create thread \n");
  //perror("");
}


void dispatch_connection_forked(int fd)
{
  pid_t child_id = fork();
  if (child_id == 0)
  {
    handle_request(fd);
    close(fd);
  }
  else if (child_id > 0)
  {  
    while (waitpid(-1, NULL, WNOHANG) > 0);
    close(fd);
  }
  else
    print_log(ERROR, "\n Neither parent nor child \n");
  //perror("");
}


int job_queue_is_full()
{
  if (cur_job_queue_size == max_job_queue_size)
    return 1;
  else
    return 0;
}


int job_queue_is_empty()
{
  if (cur_job_queue_size == 0)
    return 1;
  else
    return 0;
}


int get_job()
{
  int fd      = job_queue[first_index];
  first_index = (first_index + 1) % max_job_queue_size;
  cur_job_queue_size--;
  return fd;
}


void add_job(int fd)
{
  if (cur_job_queue_size == 0)
  {
    job_queue[0]       = fd;
    first_index        = 0;
    cur_index          = 0;
    cur_job_queue_size = 1;
  }

  else
  {
    cur_index            = (cur_index + 1) % max_job_queue_size;
    job_queue[cur_index] = fd;
    cur_job_queue_size++;
  }
}


void set_signal_mask()
{
  static sigset_t mask;
  sigemptyset(&mask);

  sigaddset (&mask, SIGUSR1);
  sigaddset (&mask, SIGUSR2);
  sigaddset (&mask, SIGTERM);
  sigaddset (&mask, SIGINT);

  pthread_sigmask (SIG_BLOCK, &mask, NULL);
}


void* consumer()
{
  set_signal_mask();
  while (status == RUNNING)
  {
    pthread_mutex_lock(&job_queue_mutex);
    while (job_queue_is_empty() == 1)
      pthread_cond_wait(&job_queue_not_empty_cond, &job_queue_mutex);

    int fd = get_job();
    pthread_cond_broadcast(&job_queue_not_full_cond);
    pthread_mutex_unlock(&job_queue_mutex);
    handle_request(fd);
    close(fd);
  }
}


void dispatch_connection_to_producer(fd)
{
  pthread_mutex_lock(&job_queue_mutex);

  while(job_queue_is_full() == 1)
    pthread_cond_wait(&job_queue_not_full_cond, &job_queue_mutex);

  print_log(DEBUG, "\n Adding job to job queue \n");
  add_job(fd);
  print_log(DEBUG, "\n Broadcasting job queue is not empty \n");
  pthread_cond_broadcast(&job_queue_not_empty_cond);
  pthread_mutex_unlock(&job_queue_mutex);
}


void dispatch_connection(int fd, http_connection_strategy_t strategy)
{
  switch (strategy)
  {
    case FORKED:
      {
        print_log(DEBUG, "\n Serving requests using forking \n");
        dispatch_connection_forked(fd);
        break;
      }

    case THREADED:
      {
        print_log(DEBUG, "\n Serving requests using threading \n");
        dispatch_connection_threaded(fd);
        break;
      }

    case SERIALIZED:
      {
        print_log(DEBUG, "\n Serving requests serially \n");
        dispatch_connection_serialized(fd);
        break;
      }

    case THREAD_POOL:
      {
        print_log(DEBUG, "\n Serving requests using thread pool \n");
        dispatch_connection_to_producer(fd);
        break;
      }
  }
}


void graceful_shutdown(int sig)
{
  //printf("%s is triggered\n", strsignal(sig));
  status = SHUTDOWN;
}


void print_statistics()
{
  print_log(DATA, "\n\t\t------------------------------------ SERVER SETTINGS ------------------------------------");
  print_log(DATA, "\n\t\t Document root    : %s", document_root);
  print_log(DATA, "\n\t\t Port number      : %d", port_num);
  switch (strategy)
  {
    case SERIALIZED:
      print_log(DATA, "\n\t\t Strategy         : Serialized");
      break;

    case THREADED:
      print_log(DATA, "\n\t\t Strategy         : Threaded");
      break;

    case FORKED: 
      print_log(DATA, "\n\t\t Strategy         : Forked");
      break;

    case THREAD_POOL:
      print_log(DATA, "\n\t\t Strategy         : Thread pool");
      print_log(DATA, "\n\t\t Thread pool size : %d", max_workers);
      print_log(DATA, "\n\t\t Buffer size      : %d", max_job_queue_size);
      break;
  }

  switch (global_log_level)
  {
    case ERROR:
      print_log(DATA, "\n\t\t Log level        : Error");
      break;

    case WARNING:
      print_log(DATA, "\n\t\t Log level        : Warning");
      break;

    case INFO: 
      print_log(DATA, "\n\t\t Log level        : Info");
      break;

    case DEBUG:
      print_log(DATA, "\n\t\t Log level        : Debug");
      break;
  }

  print_log(DATA, "\n\t\t-----------------------------------------------------------------------------------------\n");

  print_log(DATA, "\n\t\t----------------------------------- SERVER STATISTICS -----------------------------------");
  clock_gettime(CLOCK_MONOTONIC, &upend);
  print_log(DATA, "\n\t\t Total uptime                        : %.5f seconds",
      ((double)upend.tv_sec + 1.0e-9*upend.tv_nsec) - ((double)upstart.tv_sec + 1.0e-9*upstart.tv_nsec));

  if (strategy != FORKED)
  {
    print_log(DATA, "\n\t\t Total number of requests handled    : %d", total_requests_served);
    print_log(DATA, "\n\t\t Total time spent servicing requests : %.5f seconds", total_service_request_time);
    print_log(DATA, "\n\t\t Average time per request            : %.5f seconds", total_service_request_time/total_requests_served);
    print_log(DATA, "\n\t\t Total data transferred              : %d bytes", total_data_transferred);
  }
  print_log(DATA, "\n\t\t-----------------------------------------------------------------------------------------\n");
}


void increase_log_level()
{
  if (global_log_level == DEBUG)
    global_log_level = ERROR;

  else
    global_log_level++;
}


void handle_signals()
{
  struct sigaction sa, sa_sigusr1, sa_sigusr2;

  sigemptyset(&sa.sa_mask);
  sa.sa_flags   = 0;
  sa.sa_handler = graceful_shutdown;

  sigemptyset(&sa_sigusr1.sa_mask);
  sa_sigusr1.sa_flags   = 0;
  sa_sigusr1.sa_handler = print_statistics;

  sigemptyset(&sa_sigusr2.sa_mask);
  sa_sigusr2.sa_flags   = 0;
  sa_sigusr2.sa_handler = increase_log_level;

  if (sigaction(SIGINT, &sa, NULL) == -1)
    exit(EXIT_FAILURE);

  if (sigaction(SIGTERM, &sa, NULL) == -1) 
    exit(EXIT_FAILURE);

  if (sigaction(SIGUSR1, &sa_sigusr1, NULL) == -1)
    exit(EXIT_FAILURE);

  if (sigaction(SIGUSR2, &sa_sigusr2, NULL) == -1)
    exit(EXIT_FAILURE);
}


void start_server(http_connection_strategy_t strategy)
{
  int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_fd == -1)
  {
    print_log(ERROR, "\n Failed to open socket\n");
    status = SHUTDOWN;
  }

  struct sockaddr_in myaddr;
  memset(&myaddr, 0, sizeof(myaddr));
  myaddr.sin_family      = AF_INET;
  myaddr.sin_port        = htons(port_num);
  myaddr.sin_addr.s_addr = INADDR_ANY; //IP address of machine

  if (bind(sock_fd, (struct sockaddr *) &myaddr, sizeof(myaddr)) == -1)
    print_log(ERROR, "\n Failed to bind socket address\n");

  if (listen(sock_fd, LISTEN_BACKLOG) == -1)
    print_log(ERROR, "\n Listen() failed\n");

  handle_signals();

  while (status == RUNNING)
  {
    int connect_fd = accept(sock_fd, NULL, NULL);
    if (connect_fd == -1)
    {
      print_log(WARNING, "\n WARNING: Accept() failed. Interrupts may have been triggered \n");
      continue;
    }

    dispatch_connection(connect_fd, strategy);
  }

  print_log(INFO, "\n Server Shutting Down\n");
  close(sock_fd);
  free(document_root);
}


int initialize_server(int argc, char *argv[], http_connection_strategy_t *strategy)
{
  int opt;
  int count;
  int user_log_level;
  pthread_t thread_id;
  int process_flag     = 0;
  int thread_flag      = 0;
  int thread_pool_flag = 0;

  struct stat st;
  *strategy = SERIALIZED;

  while ((opt = getopt(argc, argv, "p:d:w:q:v:ft")) != -1)
  {
    switch (opt)
    {
      case 'p':
        port_num = atoi(optarg);
        break;

      case 'd':
        document_root = strdup(optarg);
        lstat(document_root, &st);
        if(!S_ISDIR(st.st_mode))
        {
          printf("<%s> is not a valid directory!\n", document_root);
          return -1;
        }
        break;

      case 'f':
        process_flag = 1;
        *strategy = FORKED;
        break;

      case 't':
        thread_flag = 1;
        *strategy = THREADED;
        break;

      case 'w':
        thread_pool_flag = 1;
        max_workers = atoi(optarg);
        *strategy   = THREAD_POOL;
        for (count = 0; count < max_workers; count++)
        {
          pthread_create(&thread_id, NULL, consumer, NULL);
          pthread_detach(thread_id);
        }

        break;

      case 'q':
        max_job_queue_size = atoi(optarg);
        break;

      case 'v':
        user_log_level = abs(atoi(optarg));
        switch (user_log_level)
        {
          case 0:
            global_log_level = ERROR;
            break;

          case 1:
            global_log_level = WARNING;
            break;

          case 2:
            global_log_level = INFO;
            break;

          case 3:
            global_log_level = DEBUG;
            break;
        }
        break;
    }
  }

  if (process_flag == 1 && thread_flag == 1)
  {
    fprintf(stderr, "Usage error: Can't use both -t and -f\n");
    return -1;
  }

  else if(thread_pool_flag == 1 && (process_flag == 1 || thread_flag == 1))
  {
    fprintf(stderr, "Usage error: Can use only one from  -t, -f and -w\n");
    return -1;
  }

  if (*strategy == THREAD_POOL)
    job_queue = malloc(max_job_queue_size * sizeof(int));

  return 0;
}


int main(int argc, char *argv[])
{
  print_log(INFO, "\n Initializing Server... \n");
  if (initialize_server(argc, argv, &strategy) < 0)
  {
    print_log(ERROR, "\n Failed to Initialize Server! Quitting...\n");
    return -1;
  }

  print_log(INFO, "\n Starting Server... \n");
  print_log(DEBUG, "\n Process ID: %d \n", getpid());

  clock_gettime(CLOCK_MONOTONIC, &upstart);
  start_server(strategy);

  return 0;
}
