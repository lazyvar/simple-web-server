/*
 * main.c
 *
 * A web server implemented on the thread pool model
 *
 */

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <netdb.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include "csapp.h"
#include "main.h"

void meminfo(int fd, char * version, char *filename, char *buf, char *filetype, char* callback);
void loadavg(int fd, char * version, char *filename, char *buf, char *filetype, char* callback);
void get_filetype(char *filename, char *filetype);
void serve_static(int fd, char * version, char *filename, int filesize, char *callback);
void sendResponseHeader(char *buf, char * version, int filesize, char *filetype);


/* global vars */
char * path;            /* root path for files */
struct list blocks;     /* blocks of anon memory */
pthread_mutex_t lock;   /* lock for list */

struct request {
    int fd;
    char * req;
    rio_t rio;
};

int main(int argc, char ** argv) {

    int c, port = 15649;
    bool pflag = false;

    /* check args, deault port is 15649 and default path is /files */
    while ((c = getopt(argc, argv, "p:R:")) != EOF) {   
        switch(c) {
            case 'p':
                port = atoi(optarg);
                break;
            case 'R':
                pflag = true;
                path = optarg;
                break;
            default:
                printf("Usage: %s [-p port] [-R path]\n", argv[0]);
                return -1;
                break;
        }
    }

    if (!pflag) {
        path = malloc(sizeof(char) * 7);
        strcpy(path, "./files");
    }

    /* supports both IPv4 and IPv6 */
    struct addrinfo hints, *ai;

    memset(&hints, '\0', sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;
    hints.ai_socktype = SOCK_STREAM;
   
    char port_char[5];
    sprintf(port_char, "%d", port);

    /* get address info and exit if error */
    if ((getaddrinfo(NULL, port_char, &hints, &ai)) != 0) {
        printf("Error getting self address info.\n");
        return -1;
    }

    /* look for IPv6 */
    struct addrinfo * r;
    for (r = ai; r != NULL; r = r->ai_next) {
        if (r->ai_family == AF_INET6) {
            ai = r;
            break;
        }
    }

    int sock;
    
    /* create socket, bind it to port specified and listen for incoming connections */
    if ((sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1) {
        printf("Error creating listening socket on port %d.\n", port);
        return -1;
    }
    
    if ((bind(sock, ai->ai_addr, ai->ai_addrlen)) != 0) {
        printf("Error binding listening socket on port %d.\n", port);
        return -1;
    }

    if ((listen(sock, SOMAXCONN)) != 0) {
        printf("Error while listening on port %d.\n", port);
        return -1;
    }

    freeaddrinfo(ai);

    /* create thread pool */
    struct thread_pool * pool;
    if ((pool = thread_pool_new(NUM_THREADS)) == NULL) {
        printf("Error creating thread pool, exiting.\n");
        return -1;
    }

    /* initialize global vars */
    list_init(&blocks);
    if ((pthread_mutex_init(&lock, NULL)) != 0) {
        printf("Error initializing lock.\n");
        return -1;
    }

    /* server loop */
    while (1) {
        struct sockaddr_storage client_addr;
        socklen_t addr_size = sizeof(client_addr);

        int fd = accept(sock, (struct sockaddr * )&client_addr, &addr_size);
    
        struct request * data = malloc(sizeof(struct request));
        data->fd = fd;
        data->req = malloc(sizeof(char) * MAXLINE);
        Rio_readinitb(&data->rio, fd);
        Rio_readlineb(&data->rio, data->req, MAXLINE);
        
        thread_pool_submit(pool, (fork_join_task_t) serve_client, (void *) data);
    }
    return 1;
}

static void * serve_client(struct thread_pool * pool, void * data) {
    struct request * request = (struct request *) data;
    
    int fd = request->fd;
    rio_t rio = request->rio;
    char * buff = request->req;
    
    char method[MAXLINE], uri[MAXLINE], version[MAXLINE];

    char trash[MAXLINE];
    Rio_readlineb(&rio, trash, MAXLINE);
    while (strcmp(trash, "\r\n")) {
        int ret = Rio_readlineb(&rio, trash, MAXLINE);
        if (ret == 0) {
            break;
        }
    }

    /* scan header info */
    sscanf(buff, "%s %s %s", method, uri, version);

    /* only implement GET */
    if (strcasecmp(method, "GET")) {
        client_error(fd, version, method, "501", "Not Implemented",
                "sysstatd does not implement this method");
        return NULL;
    }

    bool is_static;
    char filename[MAXLINE], cgiargs[MAXLINE], callback[MAXLINE], buf[MAXLINE];

    memset(filename, 0, MAXLINE);
    memset(buf, 0, MAXLINE);
    
    is_static  = parse_uri(uri, filename, cgiargs);

    if (strncmp(uri, "/loadavg", 8) == 0) {
        if (parse_for_callback(uri, callback)) { 
            strcpy(filename, "/proc/loadavg"); 
            loadavg(fd, version, filename, buf, "application/json", callback);
        } else {
            client_error(fd, version, filename, "404", "Not found",
                    "Tiny couldn't find this file");
        }
    } else if (strncmp(uri, "/meminfo", 8) == 0) {
        if (parse_for_callback(uri, callback)) {
            strcpy(filename, "/proc/meminfo"); 
            meminfo(fd, version, filename, buf, "application/json", callback);
            
        } else {
            client_error(fd, version, filename, "404", "Not found",
                    "Tiny couldn't find this file");
        }           
    } else if (strcmp(uri, "/runloop") == 0) { 
        thread_pool_submit(pool, (fork_join_task_t) runloop, (void *) data);
        char msg[MAXLINE];
        sprintf(msg, "<html><body><p>15 second loop started.</p></body></html>");
        respond_ok(fd, version, msg, "text/html", strlen(msg));
    } else if (strcmp(uri, "/allocanon") == 0) {
        allocanon(fd, version);
    } else if (strcmp(uri, "/freeanon") == 0) {
        freeanon(fd, version);
    } else {
        struct stat sbuf;

        /* invalid access */
        if (strstr(filename, "..")) {
            client_error(fd, version, filename, "403", "Forbidden",
                    "This file is forbidden");
        } else {

            char newfilename[MAXLINE];
            memset(newfilename, 0, MAXLINE);
            strcpy(newfilename, path);
            strcat(newfilename, filename);

            if (stat(newfilename, &sbuf) < 0) {                     //line:netp:doit:beginnotfound
                client_error(fd, version, filename, "404", "Not found",
                        "Tiny couldn't find this file");
            } else {                                                    //line:netp:doit:endnotfound
                if (is_static) { /* Serve static content */
		
                    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) { //line:netp:doit:readable
                        client_error(fd, version, filename, "403", "Forbidden",
                                "This file is forbidden");
                    } else {
                        serve_static(fd, version, newfilename, sbuf.st_size, callback);
                    }
                }
            }
        }
    }

    struct pollfd pfd[1];
    pfd[0].fd = fd;
    pfd[0].events = POLLIN;

    if (poll(pfd, 1, 100)) {
        char * test = malloc(sizeof(char) * MAXLINE);

        Rio_readlineb(&rio, test, MAXLINE);
      
        if (strcmp(test, "") == 0) {
            free(test);
            free(request->req);
            free(request);

            Close(fd);
            return NULL;
        } else {
            free(request->req);
            request->req = test;

            thread_pool_submit(pool, (fork_join_task_t) serve_client, data);
            return NULL;
        }
    } else {
        Close(fd);
        return NULL;
    }
}

void * runloop(struct thread_pool * pool, void * data) {
    time_t start, end;
    time(&start);

    while (difftime(end, start) < 15.0) {
        time(&end);
    }
    return NULL;
}

void allocanon(int fd, char * version) {

    char msg[MAXLINE];

    /* force allocate 256 MB of physical memory */
    void * p;
    if ((p = mmap(NULL, 268435456, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)) == MAP_FAILED) {
        sprintf(msg, "<html><body><p>Unable to allocate, mmap() failed.</p></body></html>");
        respond_ok(fd, version, msg, "text/html", strlen(msg));
        return;
    }

    /* set memory to null terminator */
    memset(p, '\0', 268435456);

    /* allocate and push onto list */
    struct block * b = malloc(sizeof(struct block));
    b->block = p;
    
    pthread_mutex_lock(&lock);
    list_push_back(&blocks, &b->elem);
    pthread_mutex_unlock(&lock);

    sprintf(msg, "<html><body><p>256MB allocated</p></body></html>");
    respond_ok(fd, version, msg, "text/html", strlen(msg));
}

void freeanon(int fd, char * version) {
    pthread_mutex_lock(&lock);
   
    char msg[MAXLINE];

    /* if list is empty return */
    if (list_empty(&blocks)) {
        pthread_mutex_unlock(&lock);
        sprintf(msg, "<html><body><p>No allocated blocks to be freed.</p></body></html>");
        respond_ok(fd, version, msg, "text/html", strlen(msg));
        return;
    }
    
    struct list_elem * be = list_pop_front(&blocks);
    pthread_mutex_unlock(&lock);
    
    struct block * b = list_entry(be, struct block, elem);

    /* attempt unmap and write out */
    if (munmap(b->block, 268435456) != 0) {
        sprintf(msg, "<html><body><p>Unable to unallocate, munmap() failed.</p></body></html>");
        respond_ok(fd, version, msg, "text/html", strlen(msg));
    } else {
        sprintf(msg, "<html><body><p>256MB succesfully freed.</p></body></html>");
        respond_ok(fd, version, msg, "text/html", strlen(msg));
    }
}

bool parse_uri(char * uri, char * filename, char * cgiargs) {
    
    char * ptr;
    
    if (!strstr(uri, "cgi-bin")) {
        strcpy(cgiargs, "");
        strcat(filename, uri);

        if (uri[strlen(uri)-1] == '/') {
            strcat(filename, "index.html");
        }
        return true;
    } else {
        ptr = index(uri, '?');
        if (ptr) {
            strcpy(cgiargs, ptr+1);
            *ptr = '\0';
        } else {
            strcpy(cgiargs, "");
        }
        
        strcat(filename, uri);
        return false;
    }
}

bool parse_for_callback(char * uri, char * callback) {
    
    char * p = uri + 8;
   
    if (*p != 63 && *p != '\0') {
        return false;
    }

    p = strstr(uri, "?callback=");

    if (p == NULL) {
        p = strstr(uri, "&callback=");
    }

    if (p == NULL) {
        strcpy(callback, "");
    } else {
        p += 10;
        char * q = p;

        int count = 0;
        while (isalnum(*q) || *q == 95 || *q == 46) {
            q++;
            count++;
        }

        memset(callback, 0, MAXLINE);

        strncpy(callback, p, count);
    }
    return true;
}

void respond_ok(int fd, char * v, char * content, char * ftype, int fsize) {
    char buff[MAXLINE];
    sprintf(buff, "%s 200 OK\r\n", v);
    sprintf(buff, "%sContent-type: %s\r\n", buff, ftype);
    sprintf(buff, "%sContent-length: %d\r\n\r\n", buff, fsize);
    Rio_writen(fd, buff, strlen(buff));
    Rio_writen(fd, content, strlen(content));
}

void client_error(int fd, char * version, char * cause, char * errnum, char * shortmsg, char * longmsg) {
    char buff[MAXLINE], body[MAXLINE];

    sprintf(body, "<html><title>Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>CS3214 Sysstatd Web server</em>", body);
    
    sprintf(buff, "%s %s %s\r\n", version, errnum, shortmsg);
    Rio_writen(fd, buff, strlen(buff));
    sprintf(buff, "Content-type: text/html\r\n");
    Rio_writen(fd, buff, strlen(buff));
    sprintf(buff, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buff, strlen(buff));
    Rio_writen(fd, body, strlen(body));
    //Close(fd);
}


void serve_static(int fd, char * version, char *filename, int filesize, char *callback)
{
    int srcfd;
    char *srcp, filetype[MAXLINE], buf[MAXBUF];

    /* Send response headers to client */

    get_filetype(filename, filetype);       //line:netp:servestatic:getfiletype
    srcfd = Open(filename, O_RDONLY, 0);    //line:netp:servestatic:open
        
    sendResponseHeader(buf, version, filesize, filetype);
    Rio_writen(fd, buf, strlen(buf));       //line:netp:servestatic:endserve
    srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);//line:netp:servestatic:mmap
    
    //Send response bod
    
    Rio_writen(fd, srcp, filesize);         //line:netp:servestatic:write
    Munmap(srcp, filesize);                 //line:netp:servestatic:munmap
    
    Close(srcfd);
}

void get_filetype(char *filename, char *filetype)
{
    if (strstr(filename, ".html"))
        strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif"))
        strcpy(filetype, "image/gif");
    else if (strstr(filename, ".jpg"))
        strcpy(filetype, "image/jpeg");
    else if(strstr(filename, ".css"))
        strcpy(filetype, "text/css");
    else if(strstr(filename, ".js"))
        strcpy(filetype, "text/javascript");
    else if(strstr(filename, "/proc"))
        strcpy(filetype, "application/json");
    else
        strcpy(filetype, "text/plain");
}

void loadavg(int fd, char * version, char *filename, char *buf, char *filetype, char *callback)
{
        char time1[MAXLINE], time2[MAXLINE], time3[MAXLINE], loadRaw[MAXLINE], load[MAXBUF], result[MAXLINE];
	    int threadsRunning, totalThreads;
        
        strcpy(load, "");
	    strcpy(result, "");
       
        if(strcmp(callback, "") !=0) {
                strcpy(result, callback);
                strcat(result, "(");
        }
        
        FILE *fp=Fopen(filename, "r");
        Fgets(loadRaw, sizeof(loadRaw), fp);
        
        sscanf(loadRaw, "%s %s %s %d/%d", time1, time2, time3, &threadsRunning, &totalThreads);
        
        sprintf(load, "{\"total_threads\": \"%d\", \"loadavg\": [\"%s\", \"%s\", \"%s\"], \"running_threads\": \"%d\"}", totalThreads, time1, time2, time3, threadsRunning);
	
        strcat(result, load);
        Fclose(fp);

        if(strcmp(callback, "") == 0)
                strcat(result, "\n");
        else
                strcat(result, ")");

        sendResponseHeader(buf, version, strlen(result), filetype);
        Rio_writen(fd, buf, strlen(buf));       //line:netp:servestatic:endserve
        //Send response body
        Rio_writen(fd, result, strlen(result));
}

void meminfo(int fd, char * version, char *filename, char *buf, char *filetype, char *callback)
{
        char memRaw[MAXLINE], mem[MAXBUF], label[MAXLINE], value[MAXLINE], line[MAXLINE];
	
        strcpy(mem, "");
        strcpy(line, "");
	
        if(strcmp(callback, "") !=0 ) {
                strcpy(mem, callback);
                strcat(mem, "(");
        }

        FILE *fp=Fopen(filename, "r");
        Fgets(memRaw, sizeof(memRaw), fp);
        sscanf(memRaw, "%s %s", label, value);
	
        label[strlen(label)-1]='\0';
        sprintf(line, "{\"%s\": \"%s\"", label, value);
        strcat(mem, line);
        
        while(Fgets(memRaw, sizeof(memRaw), fp)!=NULL){
                sscanf(memRaw, "%s %s", label, value);
                label[strlen(label)-1]='\0';
                sprintf(line, ", \"%s\": \"%s\"", label, value);
                strcat(mem, line);
        }
	
        Fclose(fp);
        
        if(strcmp(callback, "") == 0)
                strcat(mem, "}\n");
        else
                strcat(mem, "})");
        
        sendResponseHeader(buf, version, strlen(mem), filetype);
        Rio_writen(fd, buf, strlen(buf));

        Rio_writen(fd, mem, strlen(mem));

}

void sendResponseHeader(char *buf, char * version, int filesize, char *filetype)
{
        sprintf(buf, "%s 200 OK\r\n", version);    //line:netp:servestatic:beginserve
        sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
        sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
        sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
}

