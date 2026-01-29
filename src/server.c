// =============================== server.c ===============================
// Serveur TFTP simple (Partie 2)
// - écoute UDP sur port 69 (ou autre)
// - reçoit RRQ/WRQ
// - crée un socket "session" (TID) sur port éphémère
// - RRQ: envoie DATA(k) et attend ACK(k) (timeout => retransmission)
// - WRQ: envoie ACK(0), reçoit DATA(k), renvoie ACK(k) (timeout => retransmission)
//
// Important : pas d'options, pas de multi-clients, pas de threads.

#include "server.h"
#include "sockets.h"
#include "tftp_utils.h"
#include <stdio.h>
/* ---------------------------- RRQ session ---------------------------- */
static int handle_rrq(int sess_sock, const struct sockaddr_in *client,
                      const char *root_dir, const char *filename)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root_dir, filename);

    FILE *in = fopen(path, "rb");
    if (!in)
    {
        uint8_t e[256];
        int el = build_error(e, sizeof(e), 1, "File not found");
        sendto(sess_sock, e, el, 0, (struct sockaddr *)client, sizeof(*client));
        return -1;
    }

    uint8_t rx[4 + DATA_SIZE + 64];
    uint8_t last_sent[4 + DATA_SIZE];
    size_t last_len = 0;

    uint16_t block = 1;

    for (;;)
    {
        uint8_t data[DATA_SIZE];
        size_t r = fread(data, 1, DATA_SIZE, in);
        if (ferror(in))
        {
            perror("fread");
            fclose(in);
            return -1;
        }

        int dl = build_data(last_sent, sizeof(last_sent), block, data, r);
        if (dl < 0)
        {
            fclose(in);
            return -1;
        }

        sendto(sess_sock, last_sent, dl, 0, (struct sockaddr *)client, sizeof(*client));
        last_len = (size_t)dl;

        int retries = 0;
        for (;;)
        {
            struct sockaddr_in src;
            ssize_t n = recvfrom_timeout(sess_sock, rx, sizeof(rx), &src, TIMEOUT_MS);
            if (n < 0)
            {
                perror("recvfrom");
                fclose(in);
                return -1;
            }

            if (n == 0)
            {
                if (++retries > MAX_RETRIES)
                {
                    fprintf(stderr, "RRQ: timeout waiting ACK(%u)\n", block);
                    fclose(in);
                    return -1;
                }
                // retransmission du dernier DATA(block)
                sendto(sess_sock, last_sent, last_len, 0, (struct sockaddr *)client, sizeof(*client));
                continue;
            }

            // TID check: on n'accepte que l'IP:port du client qui a initié
            if (!addr_equal(&src, client))
                continue;

            uint16_t op;
            if (parse_opcode(rx, (size_t)n, &op) < 0)
                continue;
            if (op != OPCODE_ACK)
                continue;

            uint16_t ackb;
            if (parse_block(rx, (size_t)n, &ackb) < 0)
                continue;

            if (ackb == block)
                break;
        }

        if (r < DATA_SIZE)
            break; // dernier bloc
        block++;
    }

    fclose(in);
    return 0;
}

/* ---------------------------- WRQ session ---------------------------- */
static int handle_wrq(int sess_sock, const struct sockaddr_in *client,
                      const char *root_dir, const char *filename)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root_dir, filename);

    FILE *out = fopen(path, "wb");
    if (!out)
    {
        uint8_t e[256];
        int el = build_error(e, sizeof(e), 2, "Access violation");
        sendto(sess_sock, e, el, 0, (struct sockaddr *)client, sizeof(*client));
        return -1;
    }

    uint8_t rx[4 + DATA_SIZE + 64];
    uint8_t last_sent[64];
    size_t last_len = 0;

    // ACK(0) = "ok, commence à DATA(1)"
    int al = build_ack(last_sent, sizeof(last_sent), 0);
    if (al < 0)
    {
        fclose(out);
        return -1;
    }
    sendto(sess_sock, last_sent, al, 0, (struct sockaddr *)client, sizeof(*client));
    last_len = (size_t)al;

    uint16_t expected = 1;
    int retries = 0;

    for (;;)
    {
        struct sockaddr_in src;
        ssize_t n = recvfrom_timeout(sess_sock, rx, sizeof(rx), &src, TIMEOUT_MS);
        if (n < 0)
        {
            perror("recvfrom");
            fclose(out);
            return -1;
        }

        if (n == 0)
        {
            if (++retries > MAX_RETRIES)
            {
                fprintf(stderr, "WRQ: timeout waiting DATA(%u)\n", expected);
                fclose(out);
                return -1;
            }
            // retransmission du dernier ACK (ACK0 ou ACK(expected-1))
            sendto(sess_sock, last_sent, last_len, 0, (struct sockaddr *)client, sizeof(*client));
            continue;
        }

        if (!addr_equal(&src, client))
            continue;

        uint16_t op;
        if (parse_opcode(rx, (size_t)n, &op) < 0)
            continue;
        if (op != OPCODE_DATA)
            continue;

        uint16_t block;
        if (parse_block(rx, (size_t)n, &block) < 0)
            continue;

        size_t data_len = (size_t)n - 4;
        const uint8_t *data = rx + 4;

        if (block == expected)
        {
            if (fwrite(data, 1, data_len, out) != data_len)
            {
                perror("fwrite");
                fclose(out);
                return -1;
            }

            int ack_len = build_ack(last_sent, sizeof(last_sent), block);
            sendto(sess_sock, last_sent, ack_len, 0, (struct sockaddr *)client, sizeof(*client));
            last_len = (size_t)ack_len;

            retries = 0;
            expected++;

            if (data_len < DATA_SIZE)
                break; // dernier bloc
        }
        else if (block == (uint16_t)(expected - 1))
        {
            // doublon => re-ACK sans réécrire
            int ack_len = build_ack(last_sent, sizeof(last_sent), block);
            sendto(sess_sock, last_sent, ack_len, 0, (struct sockaddr *)client, sizeof(*client));
            last_len = (size_t)ack_len;
        }
    }

    fclose(out);
    return 0;
}

/* ---------------------------- Public API ---------------------------- */
int tftp_server_run(uint16_t server_port, const char *root_dir)
{
    int sock69 = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock69 < 0)
    {
        perror("socket");
        return -1;
    }

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(server_port);

    if (bind(sock69, (struct sockaddr *)&a, sizeof(a)) < 0)
    {
        perror("bind");
        close(sock69);
        return -1;
    }

    printf("TFTP server simple listening on UDP %u, root_dir=%s\n",
           (unsigned)server_port, root_dir);

    for (;;)
    {
        // 1) recevoir une requête RRQ/WRQ sur port serveur (souvent 69)
        uint8_t buf[1024];
        struct sockaddr_in client;
        socklen_t cl = sizeof(client);

        ssize_t n = recvfrom(sock69, buf, sizeof(buf), 0, (struct sockaddr *)&client, &cl);
        if (n < 0)
        {
            perror("recvfrom");
            continue;
        }

        display_packet((char *)buf, n);

        uint16_t op;
        if (parse_opcode(buf, (size_t)n, &op) < 0)
            continue;
        if (op != OPCODE_RRQ && op != OPCODE_WRQ)
            continue;

        char filename[512], mode[64];
        if (parse_rrq_wrq(buf, (size_t)n, filename, sizeof(filename), mode, sizeof(mode)) < 0)
        {
            uint8_t e[256];
            int el = build_error(e, sizeof(e), 4, "Bad RRQ/WRQ format");
            sendto(sock69, e, el, 0, (struct sockaddr *)&client, sizeof(client));
            continue;
        }

        if (!safe_name(filename))
        {
            uint8_t e[256];
            int el = build_error(e, sizeof(e), 2, "Access violation");
            sendto(sock69, e, el, 0, (struct sockaddr *)&client, sizeof(client));
            continue;
        }

        if (strcasecmp(mode, "octet") != 0)
        {
            uint8_t e[256];
            int el = build_error(e, sizeof(e), 4, "Only octet mode supported");
            sendto(sock69, e, el, 0, (struct sockaddr *)&client, sizeof(client));
            continue;
        }

        // 2) créer socket de session (TID) sur port éphémère
        int sess = socket(AF_INET, SOCK_DGRAM, 0);
        if (sess < 0)
        {
            perror("socket session");
            continue;
        }

        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = htons(0); // port éphémère
        if (bind(sess, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        {
            perror("bind session");
            close(sess);
            continue;
        }

        // 3) traiter 1 seul transfert (serveur simple)
        if (op == OPCODE_RRQ)
        {
            printf("RRQ from %s:%u file=%s\n",
                   inet_ntoa(client.sin_addr), ntohs(client.sin_port), filename);
            handle_rrq(sess, &client, root_dir, filename);
        }
        else
        {
            printf("WRQ from %s:%u file=%s\n",
                   inet_ntoa(client.sin_addr), ntohs(client.sin_port), filename);
            handle_wrq(sess, &client, root_dir, filename);
        }

        close(sess);
    }

    // never reached
    close(sock69);
    return 0;
}

int main(int argc, char **argv) {
    const char *root_dir = ".";

    if (argc >= 2)
        root_dir = argv[1];

    return tftp_server_run(69, root_dir);
}