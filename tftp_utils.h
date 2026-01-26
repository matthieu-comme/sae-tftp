#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>

#define SERVER_PORT 69
#define OPCODE_RRQ 1
#define OPCODE_WRQ 2
#define OPCODE_DATA 3
#define OPCODE_ACK 4
#define OPCODE_ERROR 5
#define DATA_SIZE 512
#define MAX_RETRIES 3

typedef struct sockaddr_in sockaddr_in;

void display_packet(const char *buffer, int size);
int build_rrq_wrq(int op_code, char *buffer, char *filename);
char *load_file(char *filename, size_t *data_size);
void send_data(int sockfd, struct sockaddr_in *addr, char *data, size_t data_size);
int init_server_addr(sockaddr_in *server_addr);