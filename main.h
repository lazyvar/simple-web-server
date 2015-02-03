#ifndef MAIN_H
#define MAIN_H

#include "list.h"
#include "csapp.h"
#include "threadpool.h"

#define NUM_THREADS 64
#define MAXLINE 8192
#define MAXBUF 8192

struct block {
    void * block;
    struct list_elem elem;
};

static void * runloop(struct thread_pool * pool, void * data);
void allocanon(int fd, char * version);
void freeanon(int fd, char * version);

bool parse_for_callback(char * uri, char * callback);
bool parse_uri(char * uri, char * filename, char * cgiargs);
static void * serve_client(struct thread_pool * pool, void * data);
void respond_ok(int fd, char * v, char * content, char * ftype, int fsize);
void client_error(int fd, char * version, char * cause, char * errnum, char * shortmsg, char * longmsg);

#endif
