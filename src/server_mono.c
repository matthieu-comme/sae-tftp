// Version mono-thread du serveur tftp multiclients

#include "server.h"
#include "sockets.h"
#include "tftp_utils.h"
#include <stdio.h>
#include <time.h>

typedef enum
{
    STATE_RRQ, // client get
    STATE_WRQ, // client put
    // attente une fois le WRQ fini, au cas où le client ne reçoit pas le ACK final et renvoie un DATA
    STATE_WAITING,
    STATE_FINISHED
} ClientState;

typedef struct ClientContext
{
    int sock;                // socket de session
    struct sockaddr_in addr; // adresse du client
    socklen_t addr_len;

    char filename[DATA_SIZE];
    FILE *fp;          // fichier ouvert
    uint16_t block;    // numero de bloc attendu (WRQ) ou dernier envoyé (RRQ)
    ClientState state; // état actuel

    uint8_t last_packet[4 + DATA_SIZE]; // copie du dernier paquet envoyé
    size_t last_packet_len;
    time_t last_activity; // pour gérer le timeout
    int retries;

    struct ClientContext *next; // liste chainée
} ClientContext;

/* ---------------------------- RRQ session ---------------------------- */

// Retourne 0 si OK, -1 si erreur critique (fermeture requise)
int step_rrq(ClientContext *ctx, uint8_t *rx_buf, ssize_t rx_len)
{
    uint16_t op;
    if (parse_opcode(rx_buf, rx_len, &op) < 0)
        return 0; // Ignorer paquet mal construit

    // vérifications opcode
    if (op == OPCODE_ERROR)
    {
        printf("Client sent ERROR. Abort.\n");
        return -1;
    }
    // en rrq, le server attend un ACK du client donc on ignore le reste
    if (op != OPCODE_ACK)
        return 0;

    // vérifications bloc
    uint16_t ack_block;
    if (parse_block(rx_buf, rx_len, &ack_block) < 0)
        return 0;

    if (ack_block == ctx->block)
    {
        // dernier paquet
        if (ctx->last_packet_len < 4 + DATA_SIZE)
        {
            ctx->state = STATE_FINISHED;
            return 0;
        }

        // bloc suivant
        ctx->block++;
        uint8_t data_read[DATA_SIZE];
        size_t read_len = fread(data_read, 1, DATA_SIZE, ctx->fp);

        if (ferror(ctx->fp))
        {
            perror("fread");
            return -1;
        }

        // construit DATA
        int len = build_data(ctx->last_packet, sizeof(ctx->last_packet), ctx->block, data_read, read_len);
        ctx->last_packet_len = len;

        sendto(ctx->sock, ctx->last_packet, len, 0, (struct sockaddr *)&ctx->addr, sizeof(ctx->addr));
        ctx->last_activity = time(NULL); // reset timer
    }
    else if (ack_block == (uint16_t)(ctx->block - 1))
    {
        // ACK précédent (perte du DATA) -> on retransmet
        sendto(ctx->sock, ctx->last_packet, ctx->last_packet_len, 0, (struct sockaddr *)&ctx->addr, sizeof(ctx->addr));
        ctx->last_activity = time(NULL);
    }

    return 0;
}

// Retourne 0 si OK, -1 si erreur
int step_wrq(ClientContext *ctx, uint8_t *rx_buf, ssize_t rx_len)
{
    uint16_t op;
    if (parse_opcode(rx_buf, rx_len, &op) < 0)
        return 0;

    if (op == OPCODE_ERROR)
        return -1;
    if (op != OPCODE_DATA)
        return 0; // ignore tout sauf erreur et data

    // verif bloc
    uint16_t data_block;
    if (parse_block(rx_buf, rx_len, &data_block) < 0)
        return 0;

    if (data_block == ctx->block)
    {
        size_t data_payload_len = rx_len - 4;
        const uint8_t *data_ptr = rx_buf + 4;
        //  écriture
        if (fwrite(data_ptr, 1, data_payload_len, ctx->fp) != data_payload_len)
        {
            return -1;
        }
        // force l'écriture pour éviter les bugs de race condition
        if (data_payload_len < DATA_SIZE)
            fflush(ctx->fp);

        // construire et envoyer ACK
        int len = build_ack(ctx->last_packet, sizeof(ctx->last_packet), ctx->block);
        ctx->last_packet_len = len;

        sendto(ctx->sock, ctx->last_packet, len, 0, (struct sockaddr *)&ctx->addr, sizeof(ctx->addr));
        ctx->last_activity = time(NULL);

        ctx->block++;

        // si dernier paquet
        if (data_payload_len < DATA_SIZE)
        {
            ctx->state = STATE_WAITING; // attend un peu avant de fermer

            // pas besoin du fichier pour renvoyer l'ACK final
            if (ctx->fp)
            {
                fflush(ctx->fp);
                fclose(ctx->fp);
                ctx->fp = NULL;
            }
        }
    }
    // bloc précédent -> doublon
    else if (data_block == (uint16_t)(ctx->block - 1))
    {
        // on renvoie l'ack précédent
        sendto(ctx->sock, ctx->last_packet, ctx->last_packet_len, 0, (struct sockaddr *)&ctx->addr, sizeof(ctx->addr));
    }

    return 0;
}

void process_client_packet(ClientContext *ctx)
{
    uint8_t buf[1024];
    struct sockaddr_in src;
    socklen_t sl = sizeof(src);

    ssize_t n = recvfrom(ctx->sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &sl);
    if (n <= 0) // erreur ou vide
        return;

    // verif TID
    if (src.sin_addr.s_addr != ctx->addr.sin_addr.s_addr ||
        src.sin_port != ctx->addr.sin_port)
    {
        return;
    }

    // aiguillage
    switch (ctx->state)
    {
    case STATE_RRQ:
        if (step_rrq(ctx, buf, n) < 0)
            ctx->state = STATE_FINISHED;
        break;
    case STATE_WRQ:
        if (step_wrq(ctx, buf, n) < 0)
            ctx->state = STATE_FINISHED;
        break;
    case STATE_WAITING:
        // si on reçoit encore DATA en waiting, on renvoie le ACK
        if (step_wrq(ctx, buf, n) < 0)
            ctx->state = STATE_FINISHED;
        break;
    default:
        break;
    }
}

// ================= GESTION LISTE CLIENTS =================

ClientContext *client_list = NULL;

ClientContext *create_client(struct sockaddr_in client_addr)
{
    ClientContext *new_client = (ClientContext *)calloc(1, sizeof(ClientContext));
    if (!new_client)
    {
        perror("malloc");
        return NULL;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        perror("socket session");
        free(new_client);
        return NULL;
    }

    // bind sur un port éphémère (0)
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(0);

    if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        perror("bind session");
        close(sock);
        free(new_client);
        return NULL;
    }

    new_client->sock = sock;
    new_client->addr = client_addr;
    new_client->addr_len = sizeof(client_addr);

    // insertion en tête de liste
    // new_client->next = client_list;
    // client_list = new_client;

    return new_client;
}

void remove_client(ClientContext *target)
{
    if (!client_list)
        return;

    if (client_list == target)
    {
        client_list = target->next;
    }
    else
    {
        ClientContext *current = client_list;
        while (current->next && current->next != target)
        {
            current = current->next;
        }
        if (current->next == target)
        {
            current->next = target->next;
        }
    }

    close(target->sock);
    free(target);
}

// cherche si un client existe déjà avec cette adresse (IP + Port)
ClientContext *find_client(struct sockaddr_in *addr)
{
    ClientContext *curr = client_list;
    while (curr != NULL)
    {
        if (curr->addr.sin_addr.s_addr == addr->sin_addr.s_addr && curr->addr.sin_port == addr->sin_port)
            return curr;

        curr = curr->next;
    }
    return NULL;
}

// Retourne 1 si l'accès est refusé (conflit), 0 si autorisé
int is_access_denied(const char *filename, uint16_t requested_op, ClientContext *exclude)
{
    ClientContext *curr = client_list;
    while (curr != NULL)
    {
        if (curr != exclude && curr->state != STATE_FINISHED && curr->state != STATE_WAITING && strcmp(curr->filename, filename) == 0)
        {
            // écriture refusée si y a deja qqn sur le fichier
            if (requested_op == OPCODE_WRQ)
            {
                return 1;
            }

            // lecture refusée si qqn écrit sur le fichier
            if (requested_op == OPCODE_RRQ && curr->state == STATE_WRQ)
            {
                return 1;
            }
        }
        curr = curr->next;
    }
    return 0; // Aucun conflit trouvé, accès autorisé
}

void handle_new_connection(int main_sock, const char *root_dir)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    uint8_t buf[1024];

    // recv paquet initial
    ssize_t n = recvfrom(main_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&client_addr, &addr_len);
    if (n <= 0)
        return;

    // si le client existe déjà, c'est qu'il a perdu notre paquet initial donc on le renvoie
    ClientContext *exist = find_client(&client_addr);
    if (exist)
    {
        if (exist->state == STATE_FINISHED || exist->state == STATE_WAITING)
        {
            char new_filename[512], new_mode[64];

            // meme fichier ?
            if (parse_rrq_wrq(buf, n, new_filename, sizeof(new_filename), new_mode, sizeof(new_mode)) == 0)
            {
                if (strcmp(new_filename, exist->filename) == 0)
                {
                    // vieux doublon -> ACK
                    sendto(exist->sock, exist->last_packet, exist->last_packet_len, 0, (struct sockaddr *)&exist->addr, exist->addr_len);
                    exist->last_activity = time(NULL);
                    return;
                }
            }

            if (exist->fp)
            {
                fclose(exist->fp);
                exist->fp = NULL;
            }
            remove_client(exist);
            exist = NULL;
        }
        else
        {
            // client actif, donc retransmission
            printf("Retransmission détectée pour [%s:%d]\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            sendto(exist->sock, exist->last_packet, exist->last_packet_len, 0, (struct sockaddr *)&exist->addr, exist->addr_len);
            exist->last_activity = time(NULL);
            return;
        }
    }

    // parsing
    uint16_t op;
    if (parse_opcode(buf, n, &op) < 0)
    {
        fprintf(stderr, "ERROR: Opcode parsing\n");
        return;
    }

    char filename[512], mode[64];
    if (parse_rrq_wrq(buf, n, filename, sizeof(filename), mode, sizeof(mode)) < 0)
    {
        fprintf(stderr, "ERROR: RRQ/WRQ parsing pour opcode %d\n", op);
        return;
    }

    if (!safe_name(filename))
    {
        printf("Refus : Nom de fichier invalide ou dangereux : %s\n", filename);
        uint8_t err[64];
        int len = build_error(err, sizeof(err), 2, "Access denied / Invalid path");
        sendto(main_sock, err, len, 0, (struct sockaddr *)&client_addr, addr_len);
        return;
    }

    ClientContext *new_c = create_client(client_addr);
    if (!new_c)
        return;

    printf("Nouveau client [%s:%d] : %s %s\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port),
           (op == OPCODE_RRQ ? "GET" : "PUT"), filename);

    // initialisation
    strncpy(new_c->filename, filename, sizeof(new_c->filename) - 1);
    new_c->last_activity = time(NULL);
    new_c->retries = 0;

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root_dir, filename);

    // traitement RRQ vs WRQ
    if (op == OPCODE_RRQ)
    {
        if (is_access_denied(filename, OPCODE_RRQ, new_c))
        {
            printf("Refus RRQ : Fichier '%s' en cours de modification (PUT).\n", filename);
            uint8_t err[64];
            int len = build_error(err, sizeof(err), 2, "File is currently locked for writing");
            sendto(main_sock, err, len, 0, (struct sockaddr *)&client_addr, addr_len);
            remove_client(new_c);
            return;
        }
        new_c->state = STATE_RRQ;
        new_c->fp = fopen(path, "rb");
        if (!new_c->fp) // fichier non trouvé -> erreur 1
        {
            uint8_t err[64];
            int len = build_error(err, sizeof(err), 1, "File not found");
            sendto(new_c->sock, err, len, 0, (struct sockaddr *)&new_c->addr, new_c->addr_len);
            new_c->state = STATE_FINISHED;

            remove_client(new_c);
            return;
        }

        // envoie DATA 1
        new_c->block = 1;
        uint8_t file_data[DATA_SIZE];
        size_t r = fread(file_data, 1, DATA_SIZE, new_c->fp);

        int len = build_data(new_c->last_packet, sizeof(new_c->last_packet), 1, file_data, r);
        new_c->last_packet_len = len;

        sendto(new_c->sock, new_c->last_packet, len, 0, (struct sockaddr *)&new_c->addr, new_c->addr_len);

        new_c->next = client_list;
        client_list = new_c;
    }
    else if (op == OPCODE_WRQ)
    {
        if (is_access_denied(filename, OPCODE_WRQ, new_c))
        {
            printf("Refus WRQ : Fichier '%s' en cours d'utilisation (GET ou PUT).\n", filename);
            uint8_t err[64];
            int len = build_error(err, sizeof(err), 2, "File is busy");
            sendto(main_sock, err, len, 0, (struct sockaddr *)&client_addr, addr_len);
            remove_client(new_c);
            return;
        }

        new_c->state = STATE_WRQ;
        new_c->fp = fopen(path, "wb");
        if (!new_c->fp) // accès refusé -> erreur 2
        {
            uint8_t err[64];
            int len = build_error(err, sizeof(err), 2, "Access denied");
            sendto(new_c->sock, err, len, 0, (struct sockaddr *)&new_c->addr, new_c->addr_len);
            new_c->state = STATE_FINISHED;
            remove_client(new_c);
            return;
        }

        // envoie ACK 0
        new_c->block = 0; // on attend le bloc 1, mais on ack le 0
        int len = build_ack(new_c->last_packet, sizeof(new_c->last_packet), 0);
        new_c->last_packet_len = len;

        sendto(new_c->sock, new_c->last_packet, len, 0, (struct sockaddr *)&new_c->addr, new_c->addr_len);
        new_c->block = 1;

        new_c->next = client_list;
        client_list = new_c;
    }
}

// ================= MAIN LOOP =================

int main(int argc, char **argv)
{
    // setbuf(stdout, NULL);
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s PORT [root_dir]\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);

    const char *root_dir = ".";
    if (argc >= 3)
        root_dir = argv[2];

    // socket d'écoute principal
    int main_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (main_sock < 0)
    {
        perror("socket main");
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    if (bind(main_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("bind main");
        close(main_sock);
        return 1;
    }

    printf("Serveur multi-clients monothread démarré sur le port %d, dossier racine: %s...\n", port, root_dir);

    // variables pour select
    fd_set readfds;
    int max_fd;
    struct timeval timeout;

    while (1)
    {
        // préparation du select
        FD_ZERO(&readfds);
        FD_SET(main_sock, &readfds);
        max_fd = main_sock;

        ClientContext *curr = client_list;
        while (curr != NULL)
        {
            FD_SET(curr->sock, &readfds);
            if (curr->sock > max_fd)
                max_fd = curr->sock;
            curr = curr->next;
        }

        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        // attente d'evenement
        int activity = select(max_fd + 1, &readfds, NULL, NULL, &timeout);

        if (activity < 0)
        {
            perror("select error");
            continue;
        }

        // Traitement des paquets reçus

        // si nouveaux clients
        if (FD_ISSET(main_sock, &readfds))
        {
            handle_new_connection(main_sock, root_dir);
        }

        // clients existants
        curr = client_list;
        while (curr != NULL)
        {
            if (FD_ISSET(curr->sock, &readfds))
                process_client_packet(curr);

            curr = curr->next;
        }

        // parcours des clients pour gérer les timeouts et supprimer les finis.
        curr = client_list;
        ClientContext *next_node = NULL;

        while (curr != NULL)
        {
            next_node = curr->next;

            time_t now = time(NULL);

            // si client fini
            if (curr->state == STATE_FINISHED)
            {
                //  garde le client x secondes pour absorber les vieux WRQ dans le buffer
                if (difftime(now, curr->last_activity) >= 15.0)
                {
                    printf("Client [%s:%d] terminé. Nettoyage.\n",
                           inet_ntoa(curr->addr.sin_addr), ntohs(curr->addr.sin_port));
                    if (curr->fp)
                        fclose(curr->fp); // Sécurité (normalement déjà fermé)
                    remove_client(curr);
                }
            }
            else if (curr->state == STATE_WAITING)
            {
                // fini si attente depuis 1 sec
                if (difftime(now, curr->last_activity) >= 1.0)
                {
                    curr->state = STATE_FINISHED;
                    curr->last_activity = now;
                }
            }

            // si timeout
            else if (difftime(now, curr->last_activity) >= 2.0) // timeout 2 secondes arbitraire
            {
                if (curr->retries >= MAX_RETRIES)
                {
                    printf("Client [%s:%d] TIMEOUT (Max retries). Suppression.\n",
                           inet_ntoa(curr->addr.sin_addr), ntohs(curr->addr.sin_port));
                    if (curr->fp)
                        fclose(curr->fp);
                    remove_client(curr);
                }
                else
                {
                    printf("Client [%s:%d] Timeout... Retransmission bloc %d\n",
                           inet_ntoa(curr->addr.sin_addr), ntohs(curr->addr.sin_port), curr->block);

                    // renvoie le dernier paquet stocké
                    sendto(curr->sock, curr->last_packet, curr->last_packet_len, 0,
                           (struct sockaddr *)&curr->addr, curr->addr_len);

                    curr->last_activity = now; // reset timer
                    curr->retries++;
                }
            }
            curr = next_node;
        }
    }

    close(main_sock);
    return 0;
}