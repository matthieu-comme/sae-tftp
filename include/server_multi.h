#ifndef SERVER_MULTI_H
#define SERVER_MULTI_H
#include <stdint.h>
int tftp_server_run_multithread(uint16_t server_port, const char *root_dir);
#endif
