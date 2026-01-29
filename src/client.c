// =============================== client.c ===============================
// - UDP + timeout(select) + retransmissions
// - Gestion TID (port session serveur)
// - RRQ/WRQ/DATA/ACK/ERROR (sans options)

#include "client.h"
#include "sockets.h"

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
                    const char *remote_file, const char *local_file)
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

    int rrq_len = build_rrq_wrq(OPCODE_RRQ, last_sent, sizeof(last_sent), remote_file);
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

    uint16_t expected = 1;
    int retries = 0;

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
                close(sock);
                return -1;
            }
            int ack_len = build_ack(last_sent, sizeof(last_sent), block);
            sendto(sock, last_sent, ack_len, 0, (struct sockaddr *)&tid, sizeof(tid));
            last_len = (size_t)ack_len;

            retries = 0;
            expected++;

            if (data_len < DATA_SIZE)
                break; // last block
        }
        else if (block == (uint16_t)(expected - 1))
        {
            // duplicate DATA -> re-ACK
            int ack_len = build_ack(last_sent, sizeof(last_sent), block);
            sendto(sock, last_sent, ack_len, 0, (struct sockaddr *)&tid, sizeof(tid));
            last_len = (size_t)ack_len;
        }
    }

    fclose(out);
    close(sock);
    return 0;
}

/* ------------------- API: PUT (WRQ) ------------------- */
int tftp_client_put(const char *server_ip, uint16_t server_port,
                    const char *local_file, const char *remote_file)
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

    int wrq_len = build_rrq_wrq(OPCODE_WRQ, last_sent, sizeof(last_sent), remote_file);
    if (wrq_len < 0)
    {
        fprintf(stderr, "WRQ build failed\n");
        fclose(in);
        close(sock);
        return -1;
    }

    sendto(sock, last_sent, wrq_len, 0, (struct sockaddr *)&srv, sizeof(srv));
    last_len = (size_t)wrq_len;

    struct sockaddr_in tid;
    memset(&tid, 0, sizeof(tid));
    int tid_known = 0;
    int retries = 0;

    // Wait ACK(0)
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
        if (op != OPCODE_ACK)
            continue;

        uint16_t b;
        if (parse_block(rx, (size_t)n, &b) < 0)
            continue;
        if (b == 0)
            break;
    }

    uint16_t block = 1;
    for (;;)
    {
        uint8_t data[DATA_SIZE];
        size_t r = fread(data, 1, DATA_SIZE, in);
        if (ferror(in))
        {
            perror("fread");
            fclose(in);
            close(sock);
            return -1;
        }

        int dl = build_data(last_sent, sizeof(last_sent), block, data, r);
        sendto(sock, last_sent, dl, 0, (struct sockaddr *)&tid, sizeof(tid));
        last_len = (size_t)dl;

        retries = 0;
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
                if (++retries > MAX_RETRIES)
                {
                    fprintf(stderr, "PUT: timeout waiting ACK(%u)\n", block);
                    fclose(in);
                    close(sock);
                    return -1;
                }
                sendto(sock, last_sent, last_len, 0, (struct sockaddr *)&tid, sizeof(tid));
                continue;
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
            if (b == block)
                break;
        }

        if (r < DATA_SIZE)
            break; // last block
        block++;
    }

    fclose(in);
    close(sock);
    return 0;
}
