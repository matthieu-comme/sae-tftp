#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
/*
# Compilation
gcc -Wall -Iinclude tests/test_server_mono.c src/tftp_utils.c src/sockets.c -o test_server_mono

# Exécution
./test_server_mono
*/

// ====================================================================
// On inclut le fichier source pour accéder aux fonctions et structures
// On renomme le main du serveur pour éviter le conflit avec le main de test
// ====================================================================
#define main server_main
#include "../src/server_mono.c"
#undef main

#ifndef DATA_SIZE
#define DATA_SIZE 512
#endif

// ================= UTILITAIRES DE TEST =================

void create_dummy_file(const char *filename, const char *content)
{
    FILE *f = fopen(filename, "wb");
    if (f)
    {
        fwrite(content, 1, strlen(content), f);
        fclose(f);
    }
}

long get_file_size(const char *filename)
{
    struct stat st;
    if (stat(filename, &st) == 0)
        return st.st_size;
    return -1;
}

// ================= TESTS =================

void test_linked_list_management()
{
    printf("[TEST] Linked List (Create/Remove)... ");

    // Reset list
    client_list = NULL;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    // 1. Création de 3 clients
    addr.sin_port = htons(1001);
    ClientContext *c1 = create_client(addr);

    addr.sin_port = htons(1002);
    ClientContext *c2 = create_client(addr);

    addr.sin_port = htons(1003);
    ClientContext *c3 = create_client(addr);

    // Vérif: L'insertion se fait en tête
    assert(client_list == c3);
    assert(client_list->next == c2);
    assert(client_list->next->next == c1);

    // 2. Suppression au milieu (c2)
    remove_client(c2);
    assert(client_list == c3);
    assert(client_list->next == c1); // c2 a disparu

    // 3. Suppression en tête (c3)
    remove_client(c3);
    assert(client_list == c1);

    // 4. Suppression dernier (c1)
    remove_client(c1);
    assert(client_list == NULL);

    printf("OK\n");
}

void test_step_rrq()
{
    printf("[TEST] RRQ Logic (GET)... ");

    // Setup fichier source
    create_dummy_file("test_rrq.tmp", "HELLO_WORLD");

    // Setup Contexte
    ClientContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = socket(AF_INET, SOCK_DGRAM, 0); // Vrai socket pour éviter crash sendto
    ctx.state = STATE_RRQ;
    ctx.block = 1; // On attend l'ACK du bloc précédent (ou init)
    ctx.fp = fopen("test_rrq.tmp", "rb");
    assert(ctx.fp != NULL);

    // On simule que le bloc 1 (précédent) était un bloc PLEIN (512 octets data + 4 header).
    // Sinon, step_rrq pense que le transfert est fini et n'incrémente pas le bloc.
    ctx.last_packet_len = 516; // 4 + DATA_SIZE

    // Simulation Packet ACK 1 reçu (Le client confirme avoir reçu le bloc 1, on veut le 2)
    // ATTENTION: Dans votre logique step_rrq:
    // "if (ack_block == ctx->block)" -> Si on reçoit ACK N, on envoie DATA N+1

    uint8_t rx_buf[4];
    // Opcode ACK (4)
    uint16_t op = htons(OPCODE_ACK);
    memcpy(rx_buf, &op, 2);
    // Block 1
    uint16_t blk = htons(1);
    memcpy(rx_buf + 2, &blk, 2);

    // --- Action ---
    int res = step_rrq(&ctx, rx_buf, 4);

    // --- Assertions ---
    assert(res == 0);
    assert(ctx.block == 2); // Doit avoir incrémenté

    // Vérifier que le paquet envoyé (stocké dans last_packet) est un DATA
    uint16_t sent_op;
    memcpy(&sent_op, ctx.last_packet, 2);
    assert(ntohs(sent_op) == OPCODE_DATA);

    // Vérifier le contenu "HELLO_WORLD" (DATA_SIZE > 11 donc tout est là)
    // Header (4) + Data
    assert(memcmp(ctx.last_packet + 4, "HELLO_WORLD", 11) == 0);

    // Vérifier état fin (taille paquet < DATA_SIZE + 4)
    // Ici "HELLO_WORLD" fait 11 octets. 11 < 512.
    // Votre code vérifie "ctx->last_packet_len < 4 + DATA_SIZE" au tour SUIVANT
    // Donc on doit simuler l'ACK 2 pour voir le passage à FINISHED.

    // Simulation ACK 2
    blk = htons(2);
    memcpy(rx_buf + 2, &blk, 2);

    res = step_rrq(&ctx, rx_buf, 4);
    assert(ctx.state == STATE_FINISHED);

    fclose(ctx.fp);
    close(ctx.sock);
    remove("test_rrq.tmp");
    printf("OK\n");
}

void test_step_wrq_normal()
{
    printf("[TEST] WRQ Logic (PUT - Normal)... ");

    // Setup
    ClientContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = socket(AF_INET, SOCK_DGRAM, 0);
    ctx.state = STATE_WRQ;
    ctx.block = 1; // On attend le bloc 1
    ctx.fp = fopen("test_wrq.tmp", "wb");

    // Simulation Packet DATA 1 "ABCD"
    uint8_t rx_buf[100];
    uint16_t op = htons(OPCODE_DATA);
    uint16_t blk = htons(1);
    memcpy(rx_buf, &op, 2);
    memcpy(rx_buf + 2, &blk, 2);
    memcpy(rx_buf + 4, "ABCD", 4);

    int res = step_wrq(&ctx, rx_buf, 4 + 4); // 4 header + 4 data

    assert(res == 0);
    assert(ctx.block == 2); // On attend maintenant le 2

    // L'écriture a été flushée sur le disque par le fclose interne de step_wrq
    assert(get_file_size("test_wrq.tmp") == 4);

    // Vérifier que l'ACK envoyé est pour le bloc 1
    uint16_t sent_op, sent_blk;
    memcpy(&sent_op, ctx.last_packet, 2);
    memcpy(&sent_blk, ctx.last_packet + 2, 2);
    assert(ntohs(sent_op) == OPCODE_ACK);
    assert(ntohs(sent_blk) == 1);

    // STATE_WAITING (car data < 512)
    assert(ctx.state == STATE_WAITING);
    assert(ctx.fp == NULL);

    close(ctx.sock);
    remove("test_wrq.tmp");
    printf("OK\n");
}

void test_step_wrq_duplicate()
{
    printf("[TEST] WRQ Logic (PUT - Duplicate)... ");

    ClientContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = socket(AF_INET, SOCK_DGRAM, 0);
    ctx.state = STATE_WRQ;
    ctx.block = 2; // On attend le bloc 2 (donc on a déjà reçu le 1)

    // Simulation : Le client renvoie le DATA 1 (doublon)
    uint8_t rx_buf[100];
    uint16_t op = htons(OPCODE_DATA);
    uint16_t blk = htons(1); // Bloc 1 alors qu'on veut le 2
    memcpy(rx_buf, &op, 2);
    memcpy(rx_buf + 2, &blk, 2);
    memcpy(rx_buf + 4, "DATA", 4);

    // Configurer last_packet comme si on avait envoyé ACK 1 précédemment
    build_ack(ctx.last_packet, sizeof(ctx.last_packet), 1);
    ctx.last_packet_len = 4;

    // --- Action ---
    int res = step_wrq(&ctx, rx_buf, 8);

    // --- Assertions ---
    assert(res == 0);
    assert(ctx.block == 2); // On attend TOUJOURS le 2, pas d'incrément

    // On doit avoir renvoyé l'ACK (test un peu aveugle ici sans mock sendto,
    // mais on vérifie que le code n'a pas planté et état stable)

    close(ctx.sock);
    printf("OK\n");
}

void test_handle_new_connection_rrq()
{
    printf("[TEST] Handle New Connection (RRQ)... ");

    // Pour tester ça, il faut simuler un socket qui a déjà reçu des données.
    // C'est dur sans `socketpair`. On va utiliser un `pipe` ou écrire sur un socket UDP local.
    // On va plutôt mocker recvfrom en envoyant un vrai paquet UDP sur le port.

    int server_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    srv_addr.sin_port = htons(0); // Port aléatoire
    bind(server_sock, (struct sockaddr *)&srv_addr, sizeof(srv_addr));

    // Récupérer le port assigné
    socklen_t len = sizeof(srv_addr);
    getsockname(server_sock, (struct sockaddr *)&srv_addr, &len);

    // Créer un client pour envoyer la requête
    int client_sock = socket(AF_INET, SOCK_DGRAM, 0);

    // Construire paquet RRQ "test_init.txt"
    char buf[100];
    int req_len = build_rrq_wrq(OPCODE_RRQ, (uint8_t *)buf, sizeof(buf), "test_init.txt", 0, 1);

    // Créer le fichier à lire
    create_dummy_file("test_init.txt", "INIT_DATA");

    // Envoyer au serveur
    sendto(client_sock, buf, req_len, 0, (struct sockaddr *)&srv_addr, sizeof(srv_addr));

    // --- Action ---
    // handle_new_connection va faire un recvfrom bloquant, mais on a déjà envoyé la data
    // donc ça ne devrait pas bloquer longtemps.
    handle_new_connection(server_sock, ".");

    // --- Assertions ---
    assert(client_list != NULL); // Client créé
    assert(client_list->state == STATE_RRQ);
    assert(strcmp(client_list->filename, "test_init.txt") == 0);
    assert(client_list->block == 1);

    // Le serveur a dû envoyer DATA 1. On vérifie sur le socket client.
    uint8_t rx[516];
    ssize_t n = recv(client_sock, rx, sizeof(rx), 0);
    assert(n > 4);

    uint16_t r_op, r_blk;
    parse_opcode(rx, n, &r_op);
    parse_block(rx, n, &r_blk);

    assert(r_op == OPCODE_DATA);
    assert(r_blk == 1);
    assert(memcmp(rx + 4, "INIT_DATA", 9) == 0);

    // Cleanup
    remove("test_init.txt");
    // close(server_sock); // handle_new_connection ne ferme pas le main_sock
    close(server_sock);
    close(client_sock);
    // Nettoyer la liste
    while (client_list)
        remove_client(client_list);

    printf("OK\n");
}

int main()
{
    printf("=== LANCEMENT DES TESTS UNITAIRES SERVEUR ===\n");

    test_linked_list_management();
    test_step_rrq();
    test_step_wrq_normal();
    test_step_wrq_duplicate();
    test_handle_new_connection_rrq();

    printf("=== TOUS LES TESTS SONT PASSÉS ===\n");
    return 0;
}