// =============================== client.c ===============================
// - UDP + timeout(select) + retransmissions
// - Gestion TID (port session serveur)
// - RRQ/WRQ/DATA/ACK/ERROR (sans options)

#include "client.h"
#include "sockets.h"
#include <stdio.h>
#include <string.h>

/* ------------------- Builders / Parsers ------------------- */

static void print_error_pkt(const uint8_t *buf, size_t len)
{
    if (len < 4)
    {
        fprintf(stderr, "TFTP ERROR (short)\n");
        return;
    }
    uint16_t code;
    memcpy(&code, buf + 2, 2);
    code = ntohs(code);
    const char *msg = (const char *)(buf + 4);
    fprintf(stderr, "TFTP ERROR %u: %.*s\n", code, (int)(len - 4), msg);
}

/* ------------------- API: GET (RRQ) ------------------- */
int tftp_client_get(const char *server_ip, uint16_t server_port,
                    const char *remote_file, const char *local_file, int use_bigfile, uint16_t window_size)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        die("socket");

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &srv.sin_addr) != 1)
    {
        fprintf(stderr, "Bad server IP\n");
        close(sock);
        return -1;
    }

    FILE *out = fopen(local_file, "wb");
    if (!out)
    {
        perror("fopen local");
        close(sock);
        return -1;
    }

    uint8_t rx[4 + DATA_SIZE + 64];
    uint8_t last_sent[4 + DATA_SIZE + 64];
    size_t last_len = 0;

    int rrq_len = build_rrq_wrq(OPCODE_RRQ, last_sent, sizeof(last_sent), remote_file, use_bigfile, window_size);
    if (rrq_len < 0)
    {
        fprintf(stderr, "RRQ build failed\n");
        fclose(out);
        close(sock);
        return -1;
    }

    if (sendto(sock, last_sent, rrq_len, 0, (struct sockaddr *)&srv, sizeof(srv)) < 0)
    {
        perror("sendto RRQ");
        fclose(out);
        close(sock);
        return -1;
    }
    last_len = (size_t)rrq_len;

    struct sockaddr_in tid;
    memset(&tid, 0, sizeof(tid));
    int tid_known = 0;
    int retries = 0;

    uint32_t expected = 1;
    int bigfile_ack = 0;
    uint16_t windowsize_ack = 1;
    uint16_t window_count = 0;

    for (;;)
    {
        struct sockaddr_in src;
        ssize_t n = recvfrom_timeout(sock, rx, sizeof(rx), &src, TIMEOUT_MS);
        if (n < 0)
        {
            perror("recvfrom");
            fclose(out);
            close(sock);
            return -1;
        }

        if (n == 0)
        {
            if (++retries > MAX_RETRIES)
            {
                fprintf(stderr, "GET: timeout (max retries)\n");
                fclose(out);
                close(sock);
                return -1;
            }
            const struct sockaddr_in *dst = tid_known ? &tid : &srv;
            sendto(sock, last_sent, last_len, 0, (struct sockaddr *)dst, sizeof(*dst));
            continue;
        }

        if (!tid_known)
        {
            tid = src;
            tid_known = 1;
        }
        else if (!addr_equal(&src, &tid))
            continue; // TID check

        uint16_t op;
        if (parse_opcode(rx, (size_t)n, &op) < 0)
            continue;

        if (op == OPCODE_ERROR)
        {
            print_error_pkt(rx, (size_t)n);
            fclose(out);
            close(sock);
            return -1;
        }
        if (op == OPCODE_OACK)
        {
            parse_oack(rx, (size_t)n, &bigfile_ack, &windowsize_ack);
            int ack_len = build_ack(last_sent, sizeof(last_sent), 0);

            sendto(sock, last_sent, ack_len, 0, (struct sockaddr *)&tid, sizeof(tid));

            last_len = (size_t)ack_len;
            retries = 0;
            continue;
        }

        if (op != OPCODE_DATA)
            continue;

        uint16_t block;
        if (parse_block(rx, (size_t)n, &block) < 0)
            continue;

        uint16_t expected_16 = (uint16_t)(expected & 0xFFFF);

        size_t data_len = (size_t)n - 4;
        const uint8_t *data = rx + 4;

        if (block == expected_16)
        {
            if (fwrite(data, 1, data_len, out) != data_len)
            {
                uint8_t err[64];
                int len = build_error(err, sizeof(err), 3, "Local disk full");
                sendto(sock, err, len, 0, (struct sockaddr *)&tid, sizeof(tid));
                perror("fwrite");
                fclose(out);
                close(sock);
                return -1;
            }

            expected++;
            window_count++;

            if (window_count >= windowsize_ack || data_len < DATA_SIZE)
            {
                int ack_len = build_ack(last_sent, sizeof(last_sent), block);
                sendto(sock, last_sent, ack_len, 0, (struct sockaddr *)&tid, sizeof(tid));
                last_len = (size_t)ack_len;
                window_count = 0;
            }

            retries = 0;

            if (data_len < DATA_SIZE)
                break; // last block
        }
        else if (block == ((uint16_t)(expected - 1) & 0xFFFF))
        {
            // duplicate DATA -> re-ACK
            int ack_len = build_ack(last_sent, sizeof(last_sent), block);
            sendto(sock, last_sent, ack_len, 0, (struct sockaddr *)&tid, sizeof(tid));
            last_len = (size_t)ack_len;
        }
    }

    fclose(out);
    close(sock);
    printf("Le fichier a bien été récupéré\n");

    return 0;
}

/* ------------------- API: PUT (WRQ) ------------------- */
int tftp_client_put(const char *server_ip, uint16_t server_port,
                    const char *local_file, const char *remote_file, int use_bigfile, uint16_t window_size)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        die("socket");

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &srv.sin_addr) != 1)
    {
        fprintf(stderr, "Bad server IP\n");
        close(sock);
        return -1;
    }

    FILE *in = fopen(local_file, "rb");
    if (!in)
    {
        perror("fopen local");
        close(sock);
        return -1;
    }

    uint8_t rx[4 + DATA_SIZE + 64];
    uint8_t last_sent[4 + DATA_SIZE + 64];
    size_t last_len = 0;

    int wrq_len = build_rrq_wrq(OPCODE_WRQ, last_sent, sizeof(last_sent), remote_file, use_bigfile, window_size);
    if (wrq_len < 0)
    {
        fprintf(stderr, "WRQ build failed\n");
        fclose(in);
        close(sock);
        return -1;
    }

    sendto(sock, last_sent, wrq_len, 0, (struct sockaddr *)&srv, sizeof(srv));
    last_len = (size_t)wrq_len;

    fprintf(stderr, "[CLIENT DEBUG - %s] WRQ envoyé. Attente de l'ACK(0)...\n", local_file);

    struct sockaddr_in tid;
    memset(&tid, 0, sizeof(tid));
    int tid_known = 0;
    int retries = 0;
    int bigfile_ack = 0;
    uint16_t windowsize_ack = 1;

    // Wait OACK ou ACK(0)
    for (;;)
    {
        struct sockaddr_in src;
        ssize_t n = recvfrom_timeout(sock, rx, sizeof(rx), &src, TIMEOUT_MS);
        if (n < 0)
        {
            perror("recvfrom");
            fclose(in);
            close(sock);
            return -1;
        }

        if (n == 0)
        {
            fprintf(stderr, "[CLIENT DEBUG - %s] TIMEOUT ! Retransmission du WRQ (Essai %d)\n", local_file, retries + 1);
            if (++retries > MAX_RETRIES)
            {
                fprintf(stderr, "PUT: timeout waiting ACK(0)\n");
                fclose(in);
                close(sock);
                return -1;
            }
            sendto(sock, last_sent, last_len, 0, (struct sockaddr *)&srv, sizeof(srv));
            continue;
        }
        fprintf(stderr, "[CLIENT DEBUG - %s] Paquet de %zd octets reçu depuis le port %d\n", local_file, n, ntohs(src.sin_port));

        if (!tid_known)
        {
            tid = src;
            tid_known = 1;
        }
        else if (!addr_equal(&src, &tid))
            continue;

        uint16_t op;
        if (parse_opcode(rx, (size_t)n, &op) < 0)
            continue;

        if (op == OPCODE_ERROR)
        {
            print_error_pkt(rx, (size_t)n);
            fclose(in);
            close(sock);
            return -1;
        }

        if (op == OPCODE_OACK)
        {
            parse_oack(rx, (size_t)n, &bigfile_ack, &windowsize_ack);
            break;
        }

        if (op != OPCODE_ACK)
            continue;

        uint16_t b;
        if (parse_block(rx, (size_t)n, &b) < 0)
            continue;
        if (b == 0)
            break;
    }

    uint32_t block = 1;
    int is_last_block = 0;

    for (;;)
    {
        uint16_t window_count = 0;
        long file_pos = ftello(in); // save la pos en cas de retransmission

        // boucle d'envoi
        while (window_count < windowsize_ack && !is_last_block)
        {
            uint8_t data[DATA_SIZE];
            size_t r = fread(data, 1, DATA_SIZE, in);
            if (ferror(in))
            {
                uint8_t err[64];
                int len = build_error(err, sizeof(err), 2, "Local read error");
                sendto(sock, err, len, 0, (struct sockaddr *)&tid, sizeof(tid));
                fclose(in);
                close(sock);
                return -1;
            }

            uint16_t block_16 = (uint16_t)(block & 0xFFFF);
            int dl = build_data(last_sent, sizeof(last_sent), block_16, data, r);

            if (dl < 0)
                return -1;

            sendto(sock, last_sent, dl, 0, (struct sockaddr *)&tid, sizeof(tid));
            last_len = (size_t)dl;

            block++;
            window_count++;

            if (r < DATA_SIZE)
                is_last_block = 1;
        }

        retries = 0;
        int ack_received = 0;

        // attend l'ack de la window
        while (!ack_received)
        {
            struct sockaddr_in src;
            ssize_t n = recvfrom_timeout(sock, rx, sizeof(rx), &src, TIMEOUT_MS);
            if (n < 0)
            {
                perror("recvfrom");
                fclose(in);
                close(sock);
                return -1;
            }

            if (n == 0)
            {
                if (++retries > MAX_RETRIES)
                {
                    fprintf(stderr, "PUT: timeout waiting ACK(%u)\n", block);
                    fclose(in);
                    close(sock);
                    return -1;
                }
                // on rewind le fichier et on renvoie la window
                fseeko(in, file_pos, SEEK_SET);
                block -= window_count;
                is_last_block = 0;
                break;
            }

            if (!addr_equal(&src, &tid))
                continue;

            uint16_t op;
            if (parse_opcode(rx, (size_t)n, &op) < 0)
                continue;

            if (op == OPCODE_ERROR)
            {
                print_error_pkt(rx, (size_t)n);
                fclose(in);
                close(sock);
                return -1;
            }
            if (op != OPCODE_ACK)
                continue;

            uint16_t b;
            if (parse_block(rx, (size_t)n, &b) < 0)
                continue;

            uint16_t expected_ack = (uint16_t)((block - 1) & 0xFFFF);

            if (b == expected_ack)
            {
                ack_received = 1;
            }
            else
            {
                continue;
                /*
                // si ack inferieur reçu -> on recule dans le fichier
                fseeko(in, file_pos, SEEK_SET);
                block -= window_count;
                is_last_block = 0;
                break;
                */
            }
        }

        if (ack_received && is_last_block)
            break;
    }

    fclose(in);
    close(sock);
    printf("Le fichier a bien été envoyé\n");
    return 0;
}

int main(int argc, char **argv)
{
    // options par défaut
    int use_bigfile = 0;
    uint16_t window_size = 1;
    int opt_index = 1;

    // update des options
    while (opt_index < argc && argv[opt_index][0] == '-')
    {
        if (strcmp(argv[opt_index], "-b") == 0)
        {
            use_bigfile = 1;
            opt_index++;
        }
        else if (strcmp(argv[opt_index], "-w") == 0 && opt_index + 1 < argc)
        {
            window_size = (uint16_t)atoi(argv[opt_index] + 1);
            opt_index += 2;
        }
        else
        {
            break;
        }
    }

    if (argc - opt_index < 5)
    {
        fprintf(stderr,
                "Usage:\n"
                "  %s [-b] [-w windowsize] get <server_ip> <port> <remote_file> <local_file>\n"
                "  %s [-b] [-w windowsize] put <server_ip> <port> <local_file> <remote_file>\n",
                argv[0], argv[0]);
        return 1;
    }

    const char *cmd = argv[opt_index];
    const char *server_ip = argv[opt_index + 1];
    uint16_t port = (uint16_t)atoi(argv[opt_index + 2]);
    const char *file1 = argv[opt_index + 3];
    const char *file2 = argv[opt_index + 4];

    if (strcmp(cmd, "get") == 0)
    {
        return tftp_client_get(server_ip, port, file1, file2, use_bigfile, window_size);
    }

    if (strcmp(cmd, "put") == 0)
    {
        return tftp_client_put(server_ip, port, file1, file2, use_bigfile, window_size);
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    return 1;
}