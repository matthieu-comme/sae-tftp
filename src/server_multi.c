// =============================== server_multi.c ===============================
// Serveur TFTP multi (Partie 3)

#include "server_multi.h"
#include "server.h"
#include "sockets.h"
#include "tftp_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>

#include <pthread.h>
#include <unistd.h>
#include <errno.h>

// header pour les fonctions de sockets
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// structure de contexte pour le thread de transfert
typedef struct {
    int sess_sock;
    struct sockaddr_in client; // adresse du client
    char root_dir[1024]; // chemin du répertoire racine
    char filename[512];
    uint16_t op; // OPCODE_RRQ ou OPCODE_WRQ
} transfer_ctx_t;

// Gestion des verrous par fichier pour eviter les conflits d'accès.
typedef struct file_lock {
    char filename[512];
    pthread_rwlock_t lock;
    struct file_lock *next;
} file_lock_t;

static file_lock_t *lock_list = NULL;
static pthread_mutex_t lock_list_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile sig_atomic_t g_stop = 0; // variable pour indiquer au serveur de s'arrêter
static void on_sigint(int sig) { // pour arrêter le serveur avec (Ctrl+C) si besoin 
    (void)sig;
    g_stop = 1;
}

// verificateur de sendto pour éviter les erreurs de socket
static int sendto_checked(int sock, const void *buf, size_t len,
                          const struct sockaddr_in *dst)
{
    ssize_t s = sendto(sock, buf, len, 0, (const struct sockaddr *)dst, sizeof(*dst));
    if (s < 0) {
        perror("sendto");
        return -1;
    }
    if ((size_t)s != len) {
        fprintf(stderr, "sendto: partial send (%zd/%zu)\n", s, len);
        return -1;
    }
    return 0;
}

// Fonction qui permet d'obtenir/créer le lock d'un fichier.
static pthread_rwlock_t *get_file_lock(const char *filename) {

    pthread_mutex_lock(&lock_list_mutex);

    file_lock_t *curr = lock_list;

    while (curr) // Cherche si le lock existe déjà
    {
        if (strcmp(curr->filename, filename) == 0)
        {
            pthread_mutex_unlock(&lock_list_mutex);
            return &curr->lock;
        }
        curr = curr->next;
    }

    file_lock_t *new_lock = malloc(sizeof(file_lock_t)); // Sinon on le crée
    if (!new_lock)
    {
        pthread_mutex_unlock(&lock_list_mutex);
        return NULL;
    }

    strncpy(new_lock->filename, filename, sizeof(new_lock->filename));
    new_lock->filename[sizeof(new_lock->filename)-1] = '\0';

    int rc = pthread_rwlock_init(&new_lock->lock, NULL);
    if (rc != 0) {
        free(new_lock);
        pthread_mutex_unlock(&lock_list_mutex);
        return NULL;
    }

    new_lock->next = lock_list;
    lock_list = new_lock;

    pthread_mutex_unlock(&lock_list_mutex);

    return &new_lock->lock;
}

// Fonction pour libérer tous les locks de fichiers
static void free_all_file_locks(void) {
    pthread_mutex_lock(&lock_list_mutex);

    file_lock_t *cur = lock_list;
    lock_list = NULL;

    pthread_mutex_unlock(&lock_list_mutex);

    while (cur) {
        file_lock_t *next = cur->next;
        pthread_rwlock_destroy(&cur->lock);
        free(cur);
        cur = next;
    }
}

// on garde la meme logique de session que dans server.c, mais on lance un thread par session pour pouvoir gérer plusieurs clients en parallèle.
/* ---------------------------- RRQ session ---------------------------- */
static int handle_rrq(int sess_sock, const struct sockaddr_in *client,
                      const char *root_dir, const char *filename)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root_dir, filename);

    pthread_rwlock_t *rwlock = get_file_lock(filename); // on récupère le lock du fichier (sinon on le crée)
    if (!rwlock)
        return -1;

    int rc = pthread_rwlock_rdlock(rwlock); // lock de lecture pour les RRQ return -1 si erreur
    if (rc != 0) {
        return -1;
    }

    FILE *in = fopen(path, "rb");
    if (!in)
    {
        uint8_t e[256];
        int el = build_error(e, sizeof(e), 1, "File not found");
        if (sendto_checked(sess_sock, e, (size_t)el, client) < 0) {
            pthread_rwlock_unlock(rwlock);
            return -1;
        }
        pthread_rwlock_unlock(rwlock);
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
            pthread_rwlock_unlock(rwlock);
            return -1;
        }

        int dl = build_data(last_sent, sizeof(last_sent), block, data, r);
        if (dl < 0)
        {
            fclose(in);
            pthread_rwlock_unlock(rwlock);
            return -1;
        }

        if (sendto_checked(sess_sock, last_sent, (size_t)dl, client) < 0) {
            fclose(in);
            pthread_rwlock_unlock(rwlock);
            return -1;
        }
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
                pthread_rwlock_unlock(rwlock);
                return -1;
            }

            if (n == 0) {
                if (++retries > MAX_RETRIES)
                {
                    fprintf(stderr, "RRQ: timeout waiting ACK(%u)\n", block);
                    fclose(in);
                    pthread_rwlock_unlock(rwlock);
                    return -1;
                }
                // retransmission du dernier DATA(block)
                if (sendto_checked(sess_sock, last_sent, last_len, client) < 0) {
                    continue;
                }
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
    pthread_rwlock_unlock(rwlock); // on libère le lock.
    return 0;
}

/* ---------------------------- WRQ session ---------------------------- */
static int handle_wrq(int sess_sock, const struct sockaddr_in *client,
                      const char *root_dir, const char *filename)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root_dir, filename);

    pthread_rwlock_t *rwlock = get_file_lock(filename); // on récupère le lock du fichier (sinon on le crée)
    if (!rwlock)
        return -1;

    int rc = pthread_rwlock_wrlock(rwlock); // lock d'écriture pour les WRQ return -1 si erreur
    if (rc != 0) {
        return -1;
    }

    FILE *out = fopen(path, "wb");
    if (!out)
    {
        uint8_t e[256];
        int el = build_error(e, sizeof(e), 2, "Access violation");
        if (sendto_checked(sess_sock, e, (size_t)el, client) < 0) {
            pthread_rwlock_unlock(rwlock);
            return -1;
        }
        pthread_rwlock_unlock(rwlock);
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
        pthread_rwlock_unlock(rwlock);
        return -1;
    }
    if (sendto_checked(sess_sock, last_sent, (size_t)al, client) < 0) {
        fclose(out);
        pthread_rwlock_unlock(rwlock);
        return -1;
    }
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
            pthread_rwlock_unlock(rwlock);
            return -1;
        }

        if (n == 0) {
            if (++retries > MAX_RETRIES)
            {
                fprintf(stderr, "WRQ: timeout waiting DATA(%u)\n", expected);
                fclose(out);
                pthread_rwlock_unlock(rwlock);
                return -1;
            }
            // retransmission du dernier ACK (ACK0 ou ACK(expected-1))
            if (sendto_checked(sess_sock, last_sent, last_len, client) < 0) {
                continue;
            }
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
                pthread_rwlock_unlock(rwlock);
                return -1;
            }

            int ack_len = build_ack(last_sent, sizeof(last_sent), block);
            if (sendto_checked(sess_sock, last_sent, (size_t)ack_len, client) < 0) {
                fclose(out);
                pthread_rwlock_unlock(rwlock);
                return -1;
            }
            last_len = (size_t)ack_len;

            retries = 0;
            expected++;

            if (data_len < DATA_SIZE)
            {
                // ACK final envoyé, on attend un peu pour voir si le client retransmet.
                // si timeout : OK
                // Si réception DATA : l'ACK a été perdu, on le renvoie.
                int wait_loops = 0;
                while (wait_loops < 3)
                {
                    struct sockaddr_in wait_src;
                    ssize_t dn = recvfrom_timeout(sess_sock, rx, sizeof(rx), &wait_src, TIMEOUT_MS);

                    if (dn == 0)
                        break; // timeout = OK
                    if (dn < 0)
                        break; // erreur socket

                    if (!addr_equal(&wait_src, client))
                        continue;

                    uint16_t dop, dblk;
                    if (parse_opcode(rx, (size_t)dn, &dop) == 0 && dop == OPCODE_DATA)
                    {
                        if (parse_block(rx, (size_t)dn, &dblk) == 0 && dblk == block)
                        {
                            // le client a renvoyé le dernier DATA, on renvoie le dernier ACK
                            if (sendto_checked(sess_sock, last_sent, last_len, client) < 0) {
                                wait_loops++;
                            }
                        }
                    }
                }
                break;
            }
        }
        else if (block == (uint16_t)(expected - 1)) // doublon, on renvoie l'ack correspondant
        {
            int ack_len = build_ack(last_sent, sizeof(last_sent), block);
            if (sendto_checked(sess_sock, last_sent, (size_t)ack_len, client) < 0) {
            } else {
                last_len = (size_t)ack_len;
            }
        }
    }
    fclose(out);
    pthread_rwlock_unlock(rwlock);
    return 0;
}

// on lit le opcode et selon la réponse (RRQ ou WRQ) on lance le thread de transfert correspondant
static void *transfer_thread(void *arg) {
    transfer_ctx_t *ctx = (transfer_ctx_t *)arg;

    if (ctx->op == OPCODE_RRQ) {
        handle_rrq(ctx->sess_sock, &ctx->client, ctx->root_dir, ctx->filename);
    } else {
        handle_wrq(ctx->sess_sock, &ctx->client, ctx->root_dir, ctx->filename);
    }

    close(ctx->sess_sock);
    free(ctx);
    return NULL;
}

// on garde la meme logique de session que dans server.c, on lance un thread par session pour pouvoir gérer plusieurs clients en parallèle.
/* ---------------------------- Public API ---------------------------- */
int tftp_server_run_multithread(uint16_t server_port, const char *root_dir) {
    int sock69 = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock69 < 0)
    {
        perror("socket");
        return -1;
    }

    // Permettre la réutilisation rapide du port (après un arrêt du serveur)
    int reuse = 1;
    if (setsockopt(sock69, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        perror("setsockopt SO_REUSEADDR");
        close(sock69);
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

    printf("TFTP multi-thread listening on UDP %u, root_dir=%s\n",
           (unsigned)server_port, root_dir);

    while (!g_stop) {

        // 1) recevoir une requête RRQ/WRQ sur port serveur (souvent 69)
        uint8_t buf[1024];
        struct sockaddr_in client;

        ssize_t n = recvfrom_timeout(sock69, buf, sizeof(buf), &client, TIMEOUT_MS);
        if (n < 0)
        {
            if (errno == EINTR) {
                if (g_stop) break;
                continue;
            }
            perror("recvfrom");
            continue;
        }
        if (n == 0) {
            continue; // timeout, on continue à écouter
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
            (void)sendto_checked(sock69, e, (size_t)el, &client); // on ignore l'erreur de sendto car le client a peut-être déjà fermé la connexion après une requete mal formée
            continue;
        }

        if (!safe_name(filename))
        {
            uint8_t e[256];
            int el = build_error(e, sizeof(e), 2, "Access violation");
            (void)sendto_checked(sock69, e, (size_t)el, &client);
            continue;
        }

        if (strcasecmp(mode, "octet") != 0)
        {
            uint8_t e[256];
            int el = build_error(e, sizeof(e), 4, "Only octet mode supported");
            (void)sendto_checked(sock69, e, (size_t)el, &client);
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

        // 3) lancement du thread de transfert selon le type de requete (RRQ ou WRQ)
        transfer_ctx_t *ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            perror("malloc");
            close(sess);
            continue;
        }

        ctx->sess_sock = sess;
        ctx->client = client;
        ctx->op = op;
        snprintf(ctx->root_dir, sizeof(ctx->root_dir), "%s", root_dir);
        snprintf(ctx->filename, sizeof(ctx->filename), "%s", filename);

        pthread_t tid;
        if (pthread_create(&tid, NULL, transfer_thread, ctx) != 0) {
            perror("pthread_create");
            close(sess);
            free(ctx);
            continue;
        }
        pthread_detach(tid);
    }

    close(sock69);
    free_all_file_locks();
    return 0;
}

int main(int argc, char **argv) {

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s PORT [root_dir]\n", argv[0]);
        return 1;
    }
    const char *root_dir = ".";

    if (argc >= 3)
        root_dir = argv[2];

    return tftp_server_run_multithread(atoi(argv[1]), root_dir);
}