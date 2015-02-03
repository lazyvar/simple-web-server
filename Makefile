# Makefile for project 4 sysstatd web server

CC = gcc
CFLAGS = -Wall -Werror -g
OBJS = main.o csapp.o threadpool.o list.o

all: sysstatd

sysstatd: $(OBJS) 
	$(CC) $(CFLAGS) -pthread -o sysstatd $(OBJS)

main.o: main.c
	$(CC) $(CFLAGS) -c main.c
csapp.o: csapp.c
	$(CC) $(CFLAGS) -c csapp.c
threadpool.o: threadpool.c
	$(CC) $(CFLAGS) -c threadpool.c
list.o: list.c
	$(CC) $(CFLAGS) -c list.c

clean:
	rm -f ~* *.o sysstatd

