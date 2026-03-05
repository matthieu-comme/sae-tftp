#ifndef TFTP_SERVER_H
#define TFTP_SERVER_H

#include <stdint.h>

int tftp_server_run(uint16_t server_port, const char *root_dir);

#endif
