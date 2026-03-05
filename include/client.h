#ifndef TFTP_CLIENT_H
#define TFTP_CLIENT_H
#include <stdint.h>

int tftp_client_get(const char *server_ip, uint16_t server_port,
                    const char *remote_file, const char *local_file, int use_bigfile, uint16_t window_size);

int tftp_client_put(const char *server_ip, uint16_t server_port,
                    const char *local_file, const char *remote_file, int use_bigfile, uint16_t window_size);

#endif
