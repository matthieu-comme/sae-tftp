// =============================== main.c ===============================
// Point d'entrée unique du projet TFTP
// - Mode client : get / put
// - Mode serveur : serveur TFTP simple
//
// Compilation :
// gcc main.c client.c server.c -o tftp
//
// Exemples :
// ./tftp client get 10.1.16.112 test.py out.py
// ./tftp client put 10.1.16.112 local.bin remote.bin
// sudo ./tftp server .

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "client.h"
#include "server.h"
#include "tftp_utils.h"

#define DEFAULT_TFTP_PORT 69

static void print_usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s client get <server_ip> <remote_file> <local_file>\n", prog);
    printf("  %s client put <server_ip> <local_file> <remote_file>\n", prog);
    printf("  %s server <root_dir>\n", prog);
}

int main(int argc, char **argv)
{

    if (argc < 2)
    {
        print_usage(argv[0]);
        return 1;
    }

    /* ===================== MODE CLIENT ===================== */
    if (strcmp(argv[1], "client") == 0)
    {

        if (argc < 6)
        {
            print_usage(argv[0]);
            return 1;
        }

        const char *cmd = argv[2];
        const char *server_ip = argv[3];

        if (strcmp(cmd, "get") == 0)
        {
            const char *remote_file = argv[4];
            const char *local_file = argv[5];

            return tftp_client_get(
                server_ip,
                DEFAULT_TFTP_PORT,
                remote_file,
                local_file);
        }

        if (strcmp(cmd, "put") == 0)
        {
            const char *local_file = argv[4];
            const char *remote_file = argv[5];

            return tftp_client_put(
                server_ip,
                DEFAULT_TFTP_PORT,
                local_file,
                remote_file);
        }

        printf("Unknown client command: %s\n", cmd);
        print_usage(argv[0]);
        return 1;
    }

    /* ===================== MODE SERVEUR ===================== */
    if (strcmp(argv[1], "server") == 0)
    {

        const char *root_dir = ".";
        if (argc >= 3)
            root_dir = argv[2];

        return tftp_server_run(DEFAULT_TFTP_PORT, root_dir);
    }

    /* ===================== ERREUR ===================== */
    printf("Unknown mode: %s\n", argv[1]);
    print_usage(argv[0]);
    return 1;
}
