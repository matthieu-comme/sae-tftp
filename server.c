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

/* Timeout avec select() : nécessaire car UDP peut perdre des paquets */
static ssize_t recvfrom_timeout(int sock, uint8_t *buf, size_t max,
                                struct sockaddr_in *src, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int r = select(sock + 1, &rfds, NULL, NULL, &tv);
    if (r < 0) return -1;
    if (r == 0) return 0; // timeout

    socklen_t sl = sizeof(*src);
    return recvfrom(sock, buf, max, 0, (struct sockaddr *)src, &sl);
}

/* --------------- Builders / Parsers (minimal) --------------- */
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

static int build_ack(uint8_t *buf, size_t max, uint16_t block) {
    if (max < 4) return -1;
    uint16_t opn = htons(OP_ACK);
    uint16_t bn  = htons(block);
    memcpy(buf, &opn, 2);
    memcpy(buf + 2, &bn, 2);
    return 4;
}

static int build_error(uint8_t *buf, size_t max, uint16_t code, const char *msg) {
    size_t m = strlen(msg);
    size_t need = 2 + 2 + m + 1;
    if (need > max) return -1;

    uint16_t opn = htons(OP_ERROR);
    uint16_t cn  = htons(code);
    memcpy(buf, &opn, 2);
    memcpy(buf + 2, &cn, 2);
    memcpy(buf + 4, msg, m);
    buf[4 + m] = 0;
    return (int)need;
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

/* RRQ/WRQ: [op(2)] [filename]\0 [mode]\0
 * Parsing “borné” pour éviter lecture hors buffer.
 */
static int parse_rrq_wrq(const uint8_t *buf, size_t len,
                         char *filename, size_t fmax,
                         char *mode, size_t mmax) {
    if (len < 4) return -1;
    size_t i = 2;

    size_t f = 0;
    while (i < len && buf[i] != 0) {
        if (f + 1 >= fmax) return -1;
        filename[f++] = (char)buf[i++];
    }
    if (i >= len || buf[i] != 0) return -1;
    filename[f] = 0;
    i++;

    size_t m = 0;
    while (i < len && buf[i] != 0) {
        if (m + 1 >= mmax) return -1;
        mode[m++] = (char)buf[i++];
    }
    if (i >= len || buf[i] != 0) return -1;
    mode[m] = 0;

    return 0;
}

/* Sécurité minimale : empêche d’accéder à ../ ou chemins absolus */
static int safe_name(const char *name) {
    if (name[0] == '/') return 0;
    if (strstr(name, "..") != NULL) return 0;
    return 1;
}

/* ---------------------------- RRQ session ---------------------------- */
static int handle_rrq(int sess_sock, const struct sockaddr_in *client,
                      const char *root_dir, const char *filename) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root_dir, filename);

    FILE *in = fopen(path, "rb");
    if (!in) {
        uint8_t e[256];
        int el = build_error(e, sizeof(e), 1, "File not found");
        sendto(sess_sock, e, el, 0, (struct sockaddr *)client, sizeof(*client));
        return -1;
    }

    uint8_t rx[4 + DATA_SIZE + 64];
    uint8_t last_sent[4 + DATA_SIZE];
    size_t last_len = 0;

    uint16_t block = 1;

    for (;;) {
        uint8_t data[DATA_SIZE];
        size_t r = fread(data, 1, DATA_SIZE, in);
        if (ferror(in)) { perror("fread"); fclose(in); return -1; }

        int dl = build_data(last_sent, sizeof(last_sent), block, data, r);
        if (dl < 0) { fclose(in); return -1; }

        sendto(sess_sock, last_sent, dl, 0, (struct sockaddr *)client, sizeof(*client));
        last_len = (size_t)dl;

        int retries = 0;
        for (;;) {
            struct sockaddr_in src;
            ssize_t n = recvfrom_timeout(sess_sock, rx, sizeof(rx), &src, TIMEOUT_MS);
            if (n < 0) { perror("recvfrom"); fclose(in); return -1; }

            if (n == 0) {
                if (++retries > MAX_RETRIES) {
                    fprintf(stderr, "RRQ: timeout waiting ACK(%u)\n", block);
                    fclose(in);
                    return -1;
                }
                // retransmission du dernier DATA(block)
                sendto(sess_sock, last_sent, last_len, 0, (struct sockaddr *)client, sizeof(*client));
                continue;
            }

            // TID check: on n'accepte que l'IP:port du client qui a initié
            if (!addr_equal(&src, client)) continue;

            uint16_t op;
            if (parse_opcode(rx, (size_t)n, &op) < 0) continue;
            if (op != OP_ACK) continue;

            uint16_t ackb;
            if (parse_block(rx, (size_t)n, &ackb) < 0) continue;

            if (ackb == block) break;
        }

        if (r < DATA_SIZE) break; // dernier bloc
        block++;
    }

    fclose(in);
    return 0;
}

/* ---------------------------- WRQ session ---------------------------- */
static int handle_wrq(int sess_sock, const struct sockaddr_in *client,
                      const char *root_dir, const char *filename) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root_dir, filename);

    FILE *out = fopen(path, "wb");
    if (!out) {
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
    if (al < 0) { fclose(out); return -1; }
    sendto(sess_sock, last_sent, al, 0, (struct sockaddr *)client, sizeof(*client));
    last_len = (size_t)al;

    uint16_t expected = 1;
    int retries = 0;

    for (;;) {
        struct sockaddr_in src;
        ssize_t n = recvfrom_timeout(sess_sock, rx, sizeof(rx), &src, TIMEOUT_MS);
        if (n < 0) { perror("recvfrom"); fclose(out); return -1; }

        if (n == 0) {
            if (++retries > MAX_RETRIES) {
                fprintf(stderr, "WRQ: timeout waiting DATA(%u)\n", expected);
                fclose(out);
                return -1;
            }
            // retransmission du dernier ACK (ACK0 ou ACK(expected-1))
            sendto(sess_sock, last_sent, last_len, 0, (struct sockaddr *)client, sizeof(*client));
            continue;
        }

        if (!addr_equal(&src, client)) continue;

        uint16_t op;
        if (parse_opcode(rx, (size_t)n, &op) < 0) continue;
        if (op != OP_DATA) continue;

        uint16_t block;
        if (parse_block(rx, (size_t)n, &block) < 0) continue;

        size_t data_len = (size_t)n - 4;
        const uint8_t *data = rx + 4;

        if (block == expected) {
            if (fwrite(data, 1, data_len, out) != data_len) {
                perror("fwrite");
                fclose(out);
                return -1;
            }

            int ack_len = build_ack(last_sent, sizeof(last_sent), block);
            sendto(sess_sock, last_sent, ack_len, 0, (struct sockaddr *)client, sizeof(*client));
            last_len = (size_t)ack_len;

            retries = 0;
            expected++;

            if (data_len < DATA_SIZE) break; // dernier bloc
        } else if (block == (uint16_t)(expected - 1)) {
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
int tftp_server_run(uint16_t server_port, const char *root_dir) {
    int sock69 = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock69 < 0) { perror("socket"); return -1; }

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(server_port);

    if (bind(sock69, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("bind");
        close(sock69);
        return -1;
    }

    printf("TFTP server simple listening on UDP %u, root_dir=%s\n",
           (unsigned)server_port, root_dir);

    for (;;) {
        // 1) recevoir une requête RRQ/WRQ sur port serveur (souvent 69)
        uint8_t buf[1024];
        struct sockaddr_in client;
        socklen_t cl = sizeof(client);

        ssize_t n = recvfrom(sock69, buf, sizeof(buf), 0, (struct sockaddr *)&client, &cl);
        if (n < 0) { perror("recvfrom"); continue; }

        uint16_t op;
        if (parse_opcode(buf, (size_t)n, &op) < 0) continue;
        if (op != OP_RRQ && op != OP_WRQ) continue;

        char filename[512], mode[64];
        if (parse_rrq_wrq(buf, (size_t)n, filename, sizeof(filename), mode, sizeof(mode)) < 0) {
            uint8_t e[256];
            int el = build_error(e, sizeof(e), 4, "Bad RRQ/WRQ format");
            sendto(sock69, e, el, 0, (struct sockaddr *)&client, sizeof(client));
            continue;
        }

        if (!safe_name(filename)) {
            uint8_t e[256];
            int el = build_error(e, sizeof(e), 2, "Access violation");
            sendto(sock69, e, el, 0, (struct sockaddr *)&client, sizeof(client));
            continue;
        }

        if (strcasecmp(mode, "octet") != 0) {
            uint8_t e[256];
            int el = build_error(e, sizeof(e), 4, "Only octet mode supported");
            sendto(sock69, e, el, 0, (struct sockaddr *)&client, sizeof(client));
            continue;
        }

        // 2) créer socket de session (TID) sur port éphémère
        int sess = socket(AF_INET, SOCK_DGRAM, 0);
        if (sess < 0) { perror("socket session"); continue; }

        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = htons(0); // port éphémère
        if (bind(sess, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            perror("bind session");
            close(sess);
            continue;
        }

        // 3) traiter 1 seul transfert (serveur simple)
        if (op == OP_RRQ) {
            printf("RRQ from %s:%u file=%s\n",
                   inet_ntoa(client.sin_addr), ntohs(client.sin_port), filename);
            handle_rrq(sess, &client, root_dir, filename);
        } else {
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
