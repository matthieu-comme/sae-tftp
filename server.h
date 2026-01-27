#ifndef TFTP_SERVER_H
#define TFTP_SERVER_H

#include <stdint.h>

/* Partie 2 :
 * Serveur TFTP simple :
 * - écoute sur server_port (par défaut 69)
 * - sert les fichiers sous root_dir
 * - 1 transfert à la fois (pas de multi-clients)
 *
 * Retour: 0 si le serveur s'est terminé proprement (en pratique: boucle infinie),
 *         -1 si erreur au démarrage.
 */
int tftp_server_run(uint16_t server_port, const char *root_dir);

#endif
