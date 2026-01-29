#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include "tftp_utils.h"

void die(const char *msg);
int addr_equal(const struct sockaddr_in *a, const struct sockaddr_in *b);
ssize_t recvfrom_timeout(int sock, uint8_t *buf, size_t max,
                         struct sockaddr_in *src, int timeout_ms);