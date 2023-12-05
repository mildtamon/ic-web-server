#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <getopt.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include "parse.h"

#include "pcsa_net.h"

#define MAXBUF 8192

char port[MAXBUF];
char root[MAXBUF];
char path[MAXBUF];
typedef struct sockaddr SA;

// THREAD
#define MAXTHREAD 256
#define MAXTASK 256

int numThreads;
int timeout;
int numTasks = 0;

pthread_t thread_pool[MAXTHREAD];

pthread_mutex_t mutexQueue = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutexParse = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t threadCondition = PTHREAD_COND_INITIALIZER;

struct survival_bag{
	struct sockaddr_storage clientAddr;
	int connFd;
    char address[MAXBUF];
};
struct survival_bag* taskQueue[MAXTASK];

// get status code and description
char* statusCode(int code) {
    if (code == 200) { return "200 OK"; }
    else if(code == 400) { return "400 Bad Request"; }
    else if (code == 404) { return "404 File Not Found"; }
    else if (code == 408) { return "408 Connection Timeouts"; }
    else if (code == 501) { return "501 Unsupported Methods"; }
    else if (code == 505) { return "505 HTTP Version Not Supported"; }
    else { return NULL; }
}

// MIME type from extension
char* mimeTypes(char* ext) {
    if (strcasecmp(ext, "html") == 0) { return "text/html"; }
    else if (strcasecmp(ext, "css") == 0) { return "text/css"; }
    else if(strcmp(ext, "txt") == 0) { return "text/plain"; }
    else if (strcasecmp(ext, "js") == 0) { return "text/javascript"; }
    else if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) { return "image/jpg"; }
    else if (strcasecmp(ext, "png") == 0) { return "image/png"; }
    else if (strcasecmp(ext, "gif") == 0) { return "image/gif"; }
    else { return NULL; }
}

int write_header(int connFd, char* path, int code) {
    char header[MAXBUF];
    char *status = statusCode(code);

    // get current date & time
    time_t rawtime;
    char date[30];      // buff size of format (dd/mm/yy hh:mm:ss)
    time(&rawtime);
    strftime(date, sizeof(date), "%d/%m/%Y %H:%M:%S", localtime(&rawtime));   // string format time

    struct stat fileStat;
    if(stat(path, &fileStat) >= 0) {    // success
        char* ext = strrchr(path, '.') + 1;

        sprintf(header,
            "HTTP/1.1 %s\r\n"
            "Date: %s\r\n"
            "Server: icws\r\n"
            "Connection: close\r\n"
            "Content-type: %s\r\n"
            "Content-length: %lld\r\n"
            "Last-Modified: %s\r\n", status, date, mimeTypes(ext), fileStat.st_size, ctime(&fileStat.st_mtime));
        write_all(connFd, header, strlen(header));
        printf("%s", header);
        return 1;
    } else {
        sprintf(header, 
            "HTTP/1.1 %s\r\n"
            "Date: %s\r\n"
            "Server: icws\r\n"
            "Connection: close\r\n\r\n",
            status, date);
        write_all(connFd, header, strlen(header));
        printf("%s\n",header); 
        return 0;
    }
}

void respond_get(int connFd, char *rootFolder, char *uri) {
	char filename[MAXBUF];

    strcpy(filename, rootFolder);               
    if (strcmp(uri, "/") == 0) {
        uri = "/index.html";
    }
    else if (uri[0] != '/') {
        strcat(filename, "/");
    }
    strcat(filename, uri);

    int fd = open(filename , O_RDONLY);

    // requested object does not exist, respond with 404.
    if (fd < 0) {
        printf("Failed to open file.\n");
        write_header(connFd, path, 404);
    }

    // struct stat (store information about files)
    struct stat st;
    fstat(fd, &st);

    write_header(connFd, filename, 200);

    char buf[MAXBUF];
    int numRead;
	while((numRead = read(fd, buf, MAXBUF)) > 0){
		write_all(connFd, buf, numRead);
	}

    close(fd);
}

void serve_http(int connFd, char* rootFolder){
    char buf[MAXBUF];
    char line[MAXBUF];

    if (!read_line(connFd, buf, MAXBUF)) 
        return ;

    printf("LOG: %s\n", buf);

    char method[MAXBUF], uri[MAXBUF], version[MAXBUF];
    sscanf(buf, "%s %s %s", method, uri, version);

    printf("method: %s uri: %s version: %s\n", method, uri, version);
   
    while(read_line(connFd, line, MAXBUF) > 0){
   	    strcat(buf, line);
   	    if(strcmp(line, "\r\n") == 0) break;
    }

    pthread_mutex_lock(&mutexParse);
    Request *request = parse(buf, MAXBUF, connFd);
    pthread_mutex_unlock(&mutexParse);

    if(request == NULL) {    // bad request
        write_header(connFd, NULL, 400);
    }

    strcpy(path, rootFolder); 
    strcat(path, request->http_uri);

    if (strcasecmp(request->http_version, "HTTP/1.1") != 0) {
        write_header(connFd, path, 505);
    } else if(strcasecmp(request->http_method, "GET") == 0) { 
        respond_get(connFd, rootFolder, request->http_uri);
    } else if(strcasecmp(request->http_method, "HEAD") == 0) {
   	    write_header(connFd, path, 200);
    } else {
        printf("501 Not Implemented\n");
        write_header(connFd, path, 501);
    }
     
    free(request->headers);
    free(request);
}	

void* handler(void *arg) {
    for (;;) {
        pthread_mutex_lock(&mutexQueue);
        while(numTasks == 0) {
            pthread_cond_wait(&threadCondition, &mutexQueue);
        }
        // dequeue
        struct survival_bag task = *taskQueue[0];
        numTasks--;
        for(int i = 0; i < numTasks; i++){
            taskQueue[i] = taskQueue[i+1];
		}
        pthread_mutex_unlock(&mutexQueue);
        serve_http(task.connFd, root);
        close(task.connFd);
    }
}

int getOption(int argc, char **argv) {
    int getopt;
    int option_index = 0;

    struct option long_options[] = {
        { "port", required_argument, 0, 'p' },
        { "root", required_argument, 0, 'r' },
        { "numThreads", required_argument, 0, 'n' },
        { "timeout", required_argument, 0, 't' },
        { NULL, 0, NULL, 0 }
    };

    while((getopt = getopt_long(argc, argv, "p:r:n:t", long_options, &option_index)) != -1) {
        switch (getopt) {
            case 'p': strcpy(port, optarg); break;
            case 'r': strcpy(root, optarg); break;
            case 'n': numThreads = atoi(optarg); break;
            case 't': timeout = atoi(optarg); break;
            default:
                printf("Invalid option\n");
                exit(0);
        }
    }
    return 1;
}

int main(int argc, char* argv[]) {

    getOption(argc, argv);

    if(strlen(port) == 0 || strlen(root) == 0 || numThreads <= 0) {
        printf("Required both --port, --root, and --numThreads\n");
        exit(0);
    }

    pthread_mutex_init(&mutexParse, NULL);

    // an fd for listening for incoming connn
    int listenFd = open_listenfd(port);
    
    // create threadpool
    for (int i = 0; i < numThreads; i++) {
        if(pthread_create(&thread_pool[i], NULL, handler, NULL) != 0){
    		printf("Fail creating thread");
    	}
    }

    for (;;) {
        struct sockaddr_storage clientAddr; // to store addr of the client
        socklen_t clientLen = sizeof(struct sockaddr_storage); // size of the above

        // ...gonna block until someone connects to our socket
        int connFd = accept(listenFd, (SA *) &clientAddr, &clientLen);

        struct survival_bag *context = (struct survival_bag *) malloc(sizeof(struct survival_bag));
        context->connFd = connFd;

        // print the address of the incoming client
        char hostBuf[MAXBUF], svcBuf[MAXBUF];
        if (getnameinfo((SA *) &clientAddr, clientLen, hostBuf, MAXBUF, svcBuf, MAXBUF, 0) == 0) 
            printf("Connection from %s:%s\n", hostBuf, svcBuf); 
        else 
            printf("Connection from UNKNOWN.");
        
        // enqueue
        pthread_mutex_lock(&mutexQueue);
       	taskQueue[numTasks] = context;
       	numTasks++;
        pthread_mutex_unlock(&mutexQueue);
        pthread_cond_signal(&threadCondition);
        // serve_http(connFd, root);
        // close(connFd);
    }

    for(int i = 0;i < numThreads;i++){
    	if(pthread_join(thread_pool[i],NULL) != 0){
    		printf("Failed to join thread");
    	}
    }

    pthread_mutex_destroy(&mutexQueue);
    pthread_mutex_destroy(&mutexParse);
    pthread_cond_destroy(&threadCondition);
    return 0;
}
