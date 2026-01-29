#ifndef TFTP_CLIENT_H
#define TFTP_CLIENT_H
#include <stdint.h>

/* Partie 1:
 * - tftp_client_get : RRQ (download)
 * - tftp_client_put : WRQ (upload)
 *
 * Retour: 0 si OK, -1 si erreur
 */
int tftp_client_get(const char *server_ip, uint16_t server_port,
                    const char *remote_file, const char *local_file);

int tftp_client_put(const char *server_ip, uint16_t server_port,
                    const char *local_file, const char *remote_file);

#endif
