WebServer
=========

This is a minimal yet functional web server. It incorporates the following concepts:
- signals
- thread pools
- multithreading
- multiprocessing
- variadic functions
- socket programming
- producer consumer solution using bounded buffer
- synchronization using mutexes and condition variables

When running from the command line, the following options are supported:
- t - use multithreading
- f - use multiprocessing
- p - port number (default is 9000)
- d - document root (default is the directory where the executable resides)
- w - use thread pool. The provided number is the number of worker threads that should be created for the pool. 
- q - indicates the size of the buffer that represents the job queue. (default is 16)
- v - set the initial log level. This determines that amount of logging to be done by the web server. The level can be changed using the SIGUSR2 signal. (default log level is WARNING). An integer value is supplied to indicate the intial log level. The values are as follows: ERROR - 0, WARNING - 1, INFO - 2, DEBUG - 3.

An example invocation of the program is as follows:
```./http_server -p 3000 -d sample_site -t -v 2```
This will run the web server in multithreaded mode and listen for requests on port 3000. The document root will be the sample_site directory and the log level will be INFO.

Another example invocation would be as follows:
```./http_server -w 10 -q 20```
This will run the web server and create create 10 threads for the thread pool. The job queue size will be 20. The default values for the port number, document root and log level will be selected. 

Only one of the strategies viz. multithreading, multiprocessing or thread pool can be selected at a time using the command line options. Selecting multiple strategies wont start the web server and an error message will be displayed. 

###Issues:
- Has been tested only on Slackware 14.0
- Source code needs to be split up into multiple files
