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
#define TIMEOUT_MS 2000

typedef struct sockaddr_in sockaddr_in;

void display_packet(const char *buffer, int size);
int build_rrq_wrq(uint16_t op_code, unsigned char *buffer, size_t buffer_size, const char *filename);
char *load_file(char *filename, size_t *data_size);
void send_data(int sockfd, struct sockaddr_in *addr, unsigned char *data, size_t data_size);
int init_server_addr(sockaddr_in *server_addr);
int build_data(uint8_t *buffer, size_t buffer_size, uint16_t block_number,
               const uint8_t *data, size_t data_len);
int build_ack(unsigned char *buffer, size_t buffer_size, uint16_t block_number);
int safe_name(const char *name);
int parse_opcode(const uint8_t *buffer, size_t buffer_size, uint16_t *opcode);
int parse_block(const uint8_t *buffer, size_t buffer_size, uint16_t *block_number);
int build_error(uint8_t *buffer, size_t buffer_size, uint16_t error_code, const char *error_msg);
int parse_rrq_wrq(const uint8_t *buffer, size_t buffer_size,
                  char *filename, size_t fmax,
                  char *mode, size_t mmax);
