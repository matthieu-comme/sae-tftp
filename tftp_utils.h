#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>

#define SERVER_PORT 69
#define RRQ 1
#define WRQ 2
#define DATA 3
#define ACK 4
#define ERROR 5
#define BUFFER_SIZE 512

typedef struct sockaddr_in sockaddr_in;

void display_packet(const char *buffer, int size);
int build_rrq_wrq(int op_code, char *buffer, char *filename);
int init_server_addr(sockaddr_in *server_addr);