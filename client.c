// =============================== client.c ===============================
// - UDP + timeout(select) + retransmissions
// - Gestion TID (port session serveur)
// - RRQ/WRQ/DATA/ACK/ERROR (sans options)

#include "client.h"

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

#define DATA_SIZE   512
#define OP_RRQ   1
#define OP_WRQ   2
#define OP_DATA  3
#define OP_ACK   4
#define OP_ERROR 5
#define TIMEOUT_MS  2000
#define MAX_RETRIES 5

static void die(const char *msg) { perror(msg); exit(EXIT_FAILURE); }

static int addr_equal(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_family == b->sin_family &&
           a->sin_addr.s_addr == b->sin_addr.s_addr &&
           a->sin_port == b->sin_port;
}

static ssize_t recvfrom_timeout(int sock, uint8_t *buf, size_t max,
                                struct sockaddr_in *src, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int r = select(sock + 1, &rfds, NULL, NULL, &tv); //y a d'autres alternative comme SO_RCVTIMEO() mais select() c'est le plus fiable pour attendre X ms
    if (r < 0) return -1;
    if (r == 0) return 0; // timeout

    socklen_t sl = sizeof(*src);
    return recvfrom(sock, buf, max, 0, (struct sockaddr *)src, &sl); // te dit qui t’a répondu (IP+port)
}

/* ------------------- Builders / Parsers ------------------- */
static int build_rrq_wrq(uint8_t *buf, size_t max, uint16_t op,
                         const char *filename, const char *mode) {
    size_t fn = strlen(filename), md = strlen(mode);
    size_t need = 2 + fn + 1 + md + 1;
    if (need > max) return -1;

    uint16_t opn = htons(op);
    memcpy(buf, &opn, 2);
    memcpy(buf + 2, filename, fn);
    buf[2 + fn] = 0;
    memcpy(buf + 2 + fn + 1, mode, md);
    buf[2 + fn + 1 + md] = 0;
    return (int)need;
}

static int build_ack(uint8_t *buf, size_t max, uint16_t block) {
    if (max < 4) return -1;
    uint16_t opn = htons(OP_ACK);
    uint16_t bn  = htons(block);  // Le réseau est en big-endian. Même si ton PC est little-endian, tu dois envoyer en network order. (j'ai rien compris)
    memcpy(buf, &opn, 2);
    memcpy(buf + 2, &bn, 2);
    return 4;
}

static int build_data(uint8_t *buf, size_t max, uint16_t block,
                      const uint8_t *data, size_t len) {
    if (len > DATA_SIZE) return -1;
    if (max < 4 + len) return -1;

    uint16_t opn = htons(OP_DATA);
    uint16_t bn  = htons(block);
    memcpy(buf, &opn, 2);
    memcpy(buf + 2, &bn, 2);
    memcpy(buf + 4, data, len);
    return (int)(4 + len);
}

static int parse_opcode(const uint8_t *buf, size_t len, uint16_t *op) {
    if (len < 2) return -1;
    uint16_t x;
    memcpy(&x, buf, 2);
    *op = ntohs(x);
    return 0;
}

static int parse_block(const uint8_t *buf, size_t len, uint16_t *b) {
    if (len < 4) return -1;
    uint16_t x;
    memcpy(&x, buf + 2, 2);
    *b = ntohs(x);
    return 0;
}

static void print_error_pkt(const uint8_t *buf, size_t len) {
    if (len < 4) { fprintf(stderr, "TFTP ERROR (short)\n"); return; }
    uint16_t code;
    memcpy(&code, buf + 2, 2);
    code = ntohs(code);
    const char *msg = (const char *)(buf + 4);
    fprintf(stderr, "TFTP ERROR %u: %.*s\n", code, (int)(len - 4), msg);
}

/* ------------------- API: GET (RRQ) ------------------- */
int tftp_client_get(const char *server_ip, uint16_t server_port,
                    const char *remote_file, const char *local_file) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) die("socket");

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &srv.sin_addr) != 1) {
        fprintf(stderr, "Bad server IP\n");
        close(sock);
        return -1;
    }

    FILE *out = fopen(local_file, "wb");
    if (!out) { perror("fopen local"); close(sock); return -1; }

    uint8_t rx[4 + DATA_SIZE + 64];
    uint8_t last_sent[4 + DATA_SIZE + 64];
    size_t last_len = 0;

    int rrq_len = build_rrq_wrq(last_sent, sizeof(last_sent), OP_RRQ, remote_file, "octet");
    if (rrq_len < 0) { fprintf(stderr, "RRQ build failed\n"); fclose(out); close(sock); return -1; }

    if (sendto(sock, last_sent, rrq_len, 0, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("sendto RRQ");
        fclose(out); close(sock); return -1;
    }
    last_len = (size_t)rrq_len;

    struct sockaddr_in tid;
    memset(&tid, 0, sizeof(tid));
    int tid_known = 0;

    uint16_t expected = 1;
    int retries = 0;

    for (;;) {
        struct sockaddr_in src;
        ssize_t n = recvfrom_timeout(sock, rx, sizeof(rx), &src, TIMEOUT_MS);
        if (n < 0) { perror("recvfrom"); fclose(out); close(sock); return -1; }

        if (n == 0) {
            if (++retries > MAX_RETRIES) {
                fprintf(stderr, "GET: timeout (max retries)\n");
                fclose(out); close(sock); return -1;
            }
            const struct sockaddr_in *dst = tid_known ? &tid : &srv;
            sendto(sock, last_sent, last_len, 0, (struct sockaddr *)dst, sizeof(*dst));
            continue;
        }

        if (!tid_known) { tid = src; tid_known = 1; }
        else if (!addr_equal(&src, &tid)) continue; // TID check

        uint16_t op;
        if (parse_opcode(rx, (size_t)n, &op) < 0) continue;

        if (op == OP_ERROR) { print_error_pkt(rx, (size_t)n); fclose(out); close(sock); return -1; }
        if (op != OP_DATA) continue;

        uint16_t block;
        if (parse_block(rx, (size_t)n, &block) < 0) continue;

        size_t data_len = (size_t)n - 4;
        const uint8_t *data = rx + 4;

        if (block == expected) {
            if (fwrite(data, 1, data_len, out) != data_len) {
                perror("fwrite"); fclose(out); close(sock); return -1;
            }
            int ack_len = build_ack(last_sent, sizeof(last_sent), block);
            sendto(sock, last_sent, ack_len, 0, (struct sockaddr *)&tid, sizeof(tid));
            last_len = (size_t)ack_len;

            retries = 0;
            expected++;

            if (data_len < DATA_SIZE) break; // last block
        } else if (block == (uint16_t)(expected - 1)) {
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
                    const char *local_file, const char *remote_file) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) die("socket");

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &srv.sin_addr) != 1) {
        fprintf(stderr, "Bad server IP\n");
        close(sock);
        return -1;
    }

    FILE *in = fopen(local_file, "rb");
    if (!in) { perror("fopen local"); close(sock); return -1; }

    uint8_t rx[4 + DATA_SIZE + 64];
    uint8_t last_sent[4 + DATA_SIZE + 64];
    size_t last_len = 0;

    int wrq_len = build_rrq_wrq(last_sent, sizeof(last_sent), OP_WRQ, remote_file, "octet");
    if (wrq_len < 0) { fprintf(stderr, "WRQ build failed\n"); fclose(in); close(sock); return -1; }

    sendto(sock, last_sent, wrq_len, 0, (struct sockaddr *)&srv, sizeof(srv));
    last_len = (size_t)wrq_len;

    struct sockaddr_in tid;
    memset(&tid, 0, sizeof(tid));
    int tid_known = 0;
    int retries = 0;

    // Wait ACK(0)
    for (;;) {
        struct sockaddr_in src;
        ssize_t n = recvfrom_timeout(sock, rx, sizeof(rx), &src, TIMEOUT_MS);
        if (n < 0) { perror("recvfrom"); fclose(in); close(sock); return -1; }

        if (n == 0) {
            if (++retries > MAX_RETRIES) {
                fprintf(stderr, "PUT: timeout waiting ACK(0)\n");
                fclose(in); close(sock); return -1;
            }
            sendto(sock, last_sent, last_len, 0, (struct sockaddr *)&srv, sizeof(srv));
            continue;
        }

        if (!tid_known) { tid = src; tid_known = 1; }
        else if (!addr_equal(&src, &tid)) continue;

        uint16_t op;
        if (parse_opcode(rx, (size_t)n, &op) < 0) continue;

        if (op == OP_ERROR) { print_error_pkt(rx, (size_t)n); fclose(in); close(sock); return -1; }
        if (op != OP_ACK) continue;

        uint16_t b;
        if (parse_block(rx, (size_t)n, &b) < 0) continue;
        if (b == 0) break;
    }

    uint16_t block = 1;
    for (;;) {
        uint8_t data[DATA_SIZE];
        size_t r = fread(data, 1, DATA_SIZE, in);
        if (ferror(in)) { perror("fread"); fclose(in); close(sock); return -1; }

        int dl = build_data(last_sent, sizeof(last_sent), block, data, r);
        sendto(sock, last_sent, dl, 0, (struct sockaddr *)&tid, sizeof(tid));
        last_len = (size_t)dl;

        retries = 0;
        for (;;) {
            struct sockaddr_in src;
            ssize_t n = recvfrom_timeout(sock, rx, sizeof(rx), &src, TIMEOUT_MS);
            if (n < 0) { perror("recvfrom"); fclose(in); close(sock); return -1; }

            if (n == 0) {
                if (++retries > MAX_RETRIES) {
                    fprintf(stderr, "PUT: timeout waiting ACK(%u)\n", block);
                    fclose(in); close(sock); return -1;
                }
                sendto(sock, last_sent, last_len, 0, (struct sockaddr *)&tid, sizeof(tid));
                continue;
            }

            if (!addr_equal(&src, &tid)) continue;

            uint16_t op;
            if (parse_opcode(rx, (size_t)n, &op) < 0) continue;

            if (op == OP_ERROR) { print_error_pkt(rx, (size_t)n); fclose(in); close(sock); return -1; }
            if (op != OP_ACK) continue;

            uint16_t b;
            if (parse_block(rx, (size_t)n, &b) < 0) continue;
            if (b == block) break;
        }

        if (r < DATA_SIZE) break; // last block
        block++;
    }

    fclose(in);
    close(sock);
    return 0;
}
