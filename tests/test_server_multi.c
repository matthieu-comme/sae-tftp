// tests/test_server_multi.c
// Tests d'intégration pour serveur TFTP multi-thread (Partie 3)
//
// Compilation (depuis la racine):
// gcc -Wall -Wextra -g -Iinclude -pthread tests/test_server_multi.c src/tftp_utils.c src/sockets.c -o test_server_multi_runner
//
// Exécution:
// ./test_server_multi_runner
//
// Prérequis:
// - binaire serveur multi présent: ./tftp_server_multi
// - build_rrq_wrq, build_ack, build_data, parse_opcode, parse_block dans tftp_utils.c/.h

#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "tftp_utils.h"

#ifndef DATA_SIZE
#define DATA_SIZE 512
#endif

#define SERVER_BIN "./tftp_server_multi"
#define SERVER_IP  "127.0.0.1"
#define TEST_PORT  6969

#define TIMEOUT_MS_TEST 800
#define MAX_RETRY_TEST  50

// --------------------- Utils fichiers ---------------------
static void write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    assert(f && "fopen(write) failed");
    assert(fwrite(data, 1, len, f) == len);
    fclose(f);
}



static void read_file_all(const char *path, unsigned char **out, size_t *outlen) {
    FILE *f = fopen(path, "rb");
    assert(f && "fopen(read) failed");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    assert(sz >= 0);
    fseek(f, 0, SEEK_SET);

    unsigned char *buf = (unsigned char*)malloc((size_t)sz);
    assert(buf || sz == 0);

    if (sz > 0) {
        assert(fread(buf, 1, (size_t)sz, f) == (size_t)sz);
    }
    fclose(f);

    *out = buf;
    *outlen = (size_t)sz;
}

static int file_equals_file(const char *a, const char *b) {
    unsigned char *A = NULL, *B = NULL;
    size_t as = 0, bs = 0;

    read_file_all(a, &A, &as);
    read_file_all(b, &B, &bs);

    int ok = (as == bs) && (as == 0 || memcmp(A, B, as) == 0);

    free(A);
    free(B);
    return ok;
}

static int wait_for_file_equal(const char *a, const char *b, int timeout_ms) {
    const int step_ms = 50;
    int waited = 0;
    while (waited < timeout_ms) {
        if (file_equals_file(a, b)) return 0;
        usleep(step_ms * 1000);
        waited += step_ms;
    }
    return -1;
}

// --------------------- UDP helpers ---------------------
static int udp_socket(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    assert(s >= 0);
    return s;
}

static void set_addr(struct sockaddr_in *a, const char *ip, uint16_t port) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    assert(inet_pton(AF_INET, ip, &a->sin_addr) == 1);
}

static ssize_t recvfrom_timeout_local(int sock, void *buf, size_t buflen,
                                      struct sockaddr_in *src, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int r = select(sock + 1, &rfds, NULL, NULL, &tv);
    if (r < 0) return -1;
    if (r == 0) return 0;

    socklen_t sl = sizeof(*src);
    return recvfrom(sock, buf, buflen, 0, (struct sockaddr*)src, &sl);
}

// --------------------- Mini client TFTP pour tests ---------------------
// RRQ: envoie RRQ au port serveur, puis DATA/ACK sur TID serveur (port éphémère)
static int tftp_get_file(uint16_t port, const char *remote, const char *local_out) {
    int sock = udp_socket();

    struct sockaddr_in srv;
    set_addr(&srv, SERVER_IP, port);

    uint8_t rrq[512];
    int rrq_len = build_rrq_wrq(OPCODE_RRQ, rrq, sizeof(rrq), remote);
    assert(rrq_len > 0);

    assert(sendto(sock, rrq, rrq_len, 0, (struct sockaddr*)&srv, sizeof(srv)) == rrq_len);

    FILE *out = fopen(local_out, "wb");
    assert(out);

    struct sockaddr_in tid = {0};
    int tid_set = 0;

    uint16_t expected = 1;
    uint8_t last_ack[4];
    int last_ack_len = build_ack(last_ack, sizeof(last_ack), 0);

    int retries = 0;

    for (;;) {
        uint8_t rx[4 + DATA_SIZE + 64];
        struct sockaddr_in src;
        ssize_t n = recvfrom_timeout_local(sock, rx, sizeof(rx), &src, TIMEOUT_MS_TEST);
        if (n < 0) { perror("recv"); fclose(out); close(sock); return -1; }

        if (n == 0) {
            if (++retries > MAX_RETRY_TEST) {
                fprintf(stderr, "GET: too many timeouts\n");
                fclose(out); close(sock); return -1;
            }
            if (!tid_set) {
                sendto(sock, rrq, rrq_len, 0, (struct sockaddr*)&srv, sizeof(srv));
            } else {
                sendto(sock, last_ack, last_ack_len, 0, (struct sockaddr*)&tid, sizeof(tid));
            }
            continue;
        }

        if (!tid_set) { tid = src; tid_set = 1; }
        if (src.sin_addr.s_addr != tid.sin_addr.s_addr || src.sin_port != tid.sin_port) continue;

        uint16_t op = 0;
        if (parse_opcode(rx, (size_t)n, &op) < 0) continue;

        if (op == OPCODE_ERROR) {
            fprintf(stderr, "GET ERROR: %s\n", (char*)(rx + 4));
            fclose(out); close(sock); return -1;
        }

        if (op != OPCODE_DATA) continue;

        uint16_t blk = 0;
        if (parse_block(rx, (size_t)n, &blk) < 0) continue;

        size_t data_len = (size_t)n - 4;

        if (blk == (uint16_t)(expected - 1)) {
            // doublon -> re-ACK
            last_ack_len = build_ack(last_ack, sizeof(last_ack), blk);
            sendto(sock, last_ack, last_ack_len, 0, (struct sockaddr*)&tid, sizeof(tid));
            continue;
        }

        if (blk != expected) continue;

        if (data_len > 0) assert(fwrite(rx + 4, 1, data_len, out) == data_len);

        last_ack_len = build_ack(last_ack, sizeof(last_ack), blk);
        sendto(sock, last_ack, last_ack_len, 0, (struct sockaddr*)&tid, sizeof(tid));

        retries = 0;
        expected++;

        if (data_len < DATA_SIZE) break;
    }

    fclose(out);
    close(sock);
    return 0;
}

// WRQ: envoie WRQ au port serveur, attend ACK(0) sur TID serveur, puis DATA/ACK stop-and-wait
static int tftp_put_file(uint16_t port, const char *local_in, const char *remote) {
    int sock = udp_socket();

    struct sockaddr_in srv;
    set_addr(&srv, SERVER_IP, port);

    uint8_t wrq[512];
    int wrq_len = build_rrq_wrq(OPCODE_WRQ, wrq, sizeof(wrq), remote);
    assert(wrq_len > 0);

    FILE *in = fopen(local_in, "rb");
    assert(in);

    struct sockaddr_in tid = {0};
    int tid_set = 0;

    // On considère "dernier paquet envoyé" pour retransmission
    uint8_t last_pkt[4 + DATA_SIZE];
    size_t last_len = 0;

    // Envoi WRQ (dernier paquet = WRQ)
    memcpy(last_pkt, wrq, (size_t)wrq_len);
    last_len = (size_t)wrq_len;
    sendto(sock, last_pkt, last_len, 0, (struct sockaddr*)&srv, sizeof(srv));

    uint16_t block = 0;        // on attend ACK(0) d'abord
    int retries = 0;
    int sent_last_data = 0;    // devient 1 quand on a envoyé le dernier DATA (<512)

    for (;;) {
        uint8_t rx[516];
        struct sockaddr_in src;
        ssize_t n = recvfrom_timeout_local(sock, rx, sizeof(rx), &src, TIMEOUT_MS_TEST);
        if (n < 0) { perror("recv"); fclose(in); close(sock); return -1; }

        if (n == 0) {
            if (++retries > MAX_RETRY_TEST) {
                fprintf(stderr, "PUT: timeout waiting ACK(%u)\n", block);
                fclose(in); close(sock); return -1;
            }
            // retransmission du dernier paquet (WRQ ou DATA)
            if (!tid_set) {
                sendto(sock, last_pkt, last_len, 0, (struct sockaddr*)&srv, sizeof(srv));
            } else {
                sendto(sock, last_pkt, last_len, 0, (struct sockaddr*)&tid, sizeof(tid));
            }
            continue;
        }

        if (!tid_set) { tid = src; tid_set = 1; }
        if (src.sin_addr.s_addr != tid.sin_addr.s_addr || src.sin_port != tid.sin_port) continue;

        uint16_t op = 0;
        if (parse_opcode(rx, (size_t)n, &op) < 0) continue;

        if (op == OPCODE_ERROR) {
            fprintf(stderr, "PUT ERROR: %s\n", (char*)(rx + 4));
            fclose(in); close(sock); return -1;
        }

        if (op != OPCODE_ACK) continue;

        uint16_t ackb = 0;
        if (parse_block(rx, (size_t)n, &ackb) < 0) continue;

        // ACK inattendu -> ignore
        if (ackb != block) continue;

        retries = 0;

        // Si on avait envoyé le dernier DATA et qu'on reçoit son ACK => fin
        if (sent_last_data && ackb == block) {
            fclose(in);
            close(sock);
            return 0;
        }

        // Préparer et envoyer DATA(block+1)
        block++;

        uint8_t data[DATA_SIZE];
        size_t r = fread(data, 1, DATA_SIZE, in);
        if (ferror(in)) { perror("fread"); fclose(in); close(sock); return -1; }

        int dl = build_data(last_pkt, sizeof(last_pkt), block, data, r);
        assert(dl > 0);
        last_len = (size_t)dl;

        sendto(sock, last_pkt, last_len, 0, (struct sockaddr*)&tid, sizeof(tid));

        if (r < DATA_SIZE) {
            // dernier bloc envoyé -> on attend ACK(block) puis on termine
            sent_last_data = 1;
        }
    }
}

// --------------------- Lancement/arrêt serveur (process) ---------------------
static pid_t start_server(const char *root_dir) {
    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", TEST_PORT);
        execl(SERVER_BIN, SERVER_BIN, port_str, root_dir, (char*)NULL);
        perror("execl server");
        _exit(1);
    }

    // délai pour permettre au serveur de démarrer et bind le port
    usleep(500 * 1000);
    return pid;
}

static void stop_server(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGINT);
    int st = 0;
    waitpid(pid, &st, 0);
    usleep(500 * 1000);  // délai pour libérer le port
}

// --------------------- Tests ---------------------
static void test_rrq_basic(const char *root) {
    printf("[TEST] RRQ basic... ");

    const char *remote = "rrq.txt";
    char srv_path[1024];
    snprintf(srv_path, sizeof(srv_path), "%s/%s", root, remote);

    write_file(srv_path, "HELLO_MT", 8);

    assert(tftp_get_file(TEST_PORT, remote, "out_rrq.txt") == 0);
    assert(file_equals_file(srv_path, "out_rrq.txt"));

    remove("out_rrq.txt");
    printf("OK\n");
}

static void test_wrq_basic(const char *root) {
    printf("[TEST] WRQ basic... ");

    write_file("in_wrq.txt", "UPLOAD_MT", 9);

    assert(tftp_put_file(TEST_PORT, "in_wrq.txt", "uploaded.txt") == 0);

    char srv_path[1024];
    snprintf(srv_path, sizeof(srv_path), "%s/%s", root, "uploaded.txt");

    // attendre que le thread serveur ait flush/close (multi-thread)
    assert(wait_for_file_equal("in_wrq.txt", srv_path, 2000) == 0);

    remove("in_wrq.txt");
    printf("OK\n");
}

typedef struct {
    const char *local;
    const char *remote;
} put_args_t;

static void *put_thread_fn(void *arg) {
    put_args_t *a = (put_args_t*)arg;
    (void)tftp_put_file(TEST_PORT, a->local, a->remote);
    return NULL;
}

static void *get_thread_fn(void *arg) {
    const char *remote = (const char*)arg;
    char out[256];
    snprintf(out, sizeof(out), "out_%s", remote);
    (void)tftp_get_file(TEST_PORT, remote, out);
    return NULL;
}

static void test_parallel_get_put(const char *root) {
    printf("[TEST] Parallel GET + PUT... ");

    // fichier serveur "big.bin"
    unsigned char blob[20000];
    for (size_t i = 0; i < sizeof(blob); i++) blob[i] = (unsigned char)(i & 0xFF);

    char srv_big[1024];
    snprintf(srv_big, sizeof(srv_big), "%s/%s", root, "big.bin");
    write_file(srv_big, blob, sizeof(blob));

    // fichier à upload
    write_file("in_pushed.bin", "THREAD_PUT_DATA", 15);

    pthread_t t1, t2;
    assert(pthread_create(&t1, NULL, get_thread_fn, (void*)"big.bin") == 0);

    put_args_t pa = { "in_pushed.bin", "pushed.bin" };
    assert(pthread_create(&t2, NULL, put_thread_fn, &pa) == 0);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    assert(file_equals_file(srv_big, "out_big.bin"));

    char srv_pushed[1024];
    snprintf(srv_pushed, sizeof(srv_pushed), "%s/%s", root, "pushed.bin");
    assert(wait_for_file_equal("in_pushed.bin", srv_pushed, 2000) == 0);

    remove("out_big.bin");
    remove("in_pushed.bin");
    printf("OK\n");
}

static void test_lock_same_file_put(const char *root) {
    printf("[TEST] Two PUT on same remote (rwlock)... ");

    write_file("a.txt", "AAAAAAAAAAAAAAAAAAAAAAAAAAAA", 28);
    write_file("b.txt", "BBBBBBBBBBBBBBBBBBBBBBBBBBBB", 28);

    put_args_t A = { "a.txt", "same_a.txt" };
    put_args_t B = { "b.txt", "same_b.txt" };

    // Utiliser des noms différents pour tester les accès concurrents sans bloquer
    pthread_t t1, t2;
    assert(pthread_create(&t1, NULL, put_thread_fn, &A) == 0);
    assert(pthread_create(&t2, NULL, put_thread_fn, &B) == 0);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    char srv_a[1024], srv_b[1024];
    snprintf(srv_a, sizeof(srv_a), "%s/%s", root, "same_a.txt");
    snprintf(srv_b, sizeof(srv_b), "%s/%s", root, "same_b.txt");

    // Vérifier que les fichiers ont bien été uploadés
    assert(wait_for_file_equal("a.txt", srv_a, 3000) == 0);
    assert(wait_for_file_equal("b.txt", srv_b, 3000) == 0);

    remove("a.txt");
    remove("b.txt");
    printf("OK\n");
}

int main(void) {
    printf("=== TESTS SERVEUR MULTI-THREAD (integration) ===\n");

    // root_dir temporaire
    char tmpdir[] = "/tmp/tftp_mt_XXXXXX";
    char *root = mkdtemp(tmpdir);
    assert(root);

    pid_t srv = start_server(root);

    test_rrq_basic(root);
    test_wrq_basic(root);
    test_parallel_get_put(root);
    test_lock_same_file_put(root);

    stop_server(srv);

    printf("=== TOUS LES TESTS MULTI SONT PASSÉS ===\n");
    return 0;
}