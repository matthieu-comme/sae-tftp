#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>
#include "tftp_utils.h"
#include <sys/stat.h>

// ====================================================================
// On inclut le fichier source pour accéder aux fonctions et structures
// On renomme le main du serveur pour éviter le conflit avec le main de test
// ====================================================================

#define main server_main
#include "../src/server.c"
#undef main

// pour afficher le buffer en cas d'erreur
void print_hex(char *buffer, int size)
{
    for (int i = 0; i < size; i++)
    {
        printf("%02x ", (unsigned char)buffer[i]);
    }
    printf("\n");
}
// ----- build_rrq_wrq() -----
void test_rrq_success()
{
    printf("Test: RRQ valide (sans option)... ");
    unsigned char buffer[DATA_SIZE];
    char *fname = "test_rrq.txt";
    int size = build_rrq_wrq(OPCODE_RRQ, buffer, sizeof(buffer), fname, 0, 1);

    assert(size == 2 + ((int)strlen(fname) + 1) + 5 + 1);                   // Opcode + filename\0 + octet\0
    assert(strcmp((char *)(buffer + 2), fname) == 0);                       // filename
    assert(strcmp((char *)(buffer + 2 + strlen(fname) + 1), "octet") == 0); // Mode correct
    printf("OK\n");
}

void test_wrq_success()
{
    printf("Test: WRQ valide (sans option)... ");
    unsigned char buffer[DATA_SIZE];
    char *fname = "test_wrq.txt";
    int size = build_rrq_wrq(OPCODE_WRQ, buffer, sizeof(buffer), fname, 0, 1);

    assert(size == 2 + ((int)strlen(fname) + 1) + 6);
    assert(strcmp((char *)(buffer + 2), fname) == 0);
    assert(strcmp((char *)(buffer + 2 + strlen(fname) + 1), "octet") == 0);
    printf("OK\n");
}

void test_wrq_with_options()
{
    printf("Test: WRQ valide (avec options bigfile et windowsize)... ");
    unsigned char buffer[DATA_SIZE];
    char *fname = "test_opt_w.txt";
    int size = build_rrq_wrq(OPCODE_WRQ, buffer, sizeof(buffer), fname, 1, 8);

    int expected_size = 2 + ((int)strlen(fname) + 1) + 6 + 10 + 13; // 10="bigfile\01\0", 13="windowsize\08\0"
    assert(size == expected_size);
    printf("OK\n");
}

void test_rrq_with_options()
{
    printf("Test: RRQ valide (avec options bigfile et windowsize)... ");
    unsigned char buffer[DATA_SIZE];
    char *fname = "test_opt.txt";
    int size = build_rrq_wrq(OPCODE_RRQ, buffer, sizeof(buffer), fname, 1, 8);

    int expected_size = 2 + ((int)strlen(fname) + 1) + 6 + 10 + 13; // 10="bigfile\01\0", 13="windowsize\08\0"
    assert(size == expected_size);
    printf("OK\n");
}

void test_invalid_opcode()
{
    printf("Test: Opcode invalide (99)... ");
    unsigned char buffer[DATA_SIZE];
    int size = build_rrq_wrq(99, buffer, sizeof(buffer), "file.txt", 0, 1);
    assert(size == -1);
    printf("OK (Erreur détectée)\n");
}

void test_filename_limit()
{
    printf("Test: Nom de fichier trop long... ");
    unsigned char buffer[DATA_SIZE];
    char long_name[600];
    memset(long_name, 'a', 599);
    long_name[599] = '\0';

    int size = build_rrq_wrq(OPCODE_RRQ, buffer, sizeof(buffer), long_name, 0, 1);
    assert(size == -1);
    printf("OK (Erreur détectée)\n");
}
/*
void test_load_file()
{
    size_t size;
    char *data = load_file("toto.txt", &size);
    printf("Longueur du fichier : %ld\n", size);
    printf("%s\n", data);
}
*/

void test_build_rrq_wrq()
{
    printf("=== TESTS BUILD_RRQ_WRQ ===\n");
    test_rrq_success();
    test_wrq_success();
    test_rrq_with_options();
    test_wrq_with_options();
    test_invalid_opcode();
    test_filename_limit();
    printf("=== TOUS LES TESTS BUILD_RRQ_WRQ SONT PASSÉS ! ===\n");
}

// ----- build_oack -----
void test_oack_success()
{
    printf("Test: OACK valide... ");
    uint8_t buffer[100];
    int size = build_oack(buffer, sizeof(buffer), 1, 4);

    int expected_size = 2 + 10 + 13; // opcode(2) + bigfile\01\0(10) + windowsize\04\0(13)
    assert(size == expected_size);
    assert(buffer[0] == 0 && buffer[1] == 6); // Verif Opcode OACK (6)
    printf("OK\n");
}

void test_oack_no_options()
{
    printf("Test: OACK sans option (doit échouer/ignorer)... ");
    uint8_t buffer[100];
    int size = build_oack(buffer, sizeof(buffer), 0, 1);
    assert(size == -1);
    printf("OK\n");
}

void test_build_oack()
{
    printf("\n=== TESTS BUILD_OACK ===\n");
    test_oack_success();
    test_oack_no_options();
    printf("=== TOUS LES TESTS BUILD_OACK SONT PASSÉS ! ===\n");
}

// ----- build_data -----

void test_data_success_normal()
{
    printf("Test: DATA standard (Hello)... ");
    uint8_t buffer[1024];
    const char *msg = "Hello";
    uint16_t block_num = 12;

    int size = build_data(buffer, sizeof(buffer), block_num, (const uint8_t *)msg, strlen(msg));

    assert(size == 4 + 5); // (Header 4 + Data 5)

    assert(buffer[0] == 0 && buffer[1] == 3); // verif opcode

    assert(buffer[2] == 0 && buffer[3] == 12); // verif block_num

    assert(memcmp(buffer + 4, msg, 5) == 0); // verif msg

    printf("OK\n");
}

void test_data_max_size()
{
    printf("Test: DATA taille max (512 octets)... ");
    uint8_t buffer[1024];
    uint8_t big_data[512];
    memset(big_data, 'X', 512); // remplit de 'X'

    int size = build_data(buffer, sizeof(buffer), 1, big_data, 512);

    assert(size == 516); // Header (4) + Data (512) = 516

    assert(buffer[1] == 3);                         // opcode check rapide
    assert(memcmp(buffer + 4, big_data, 512) == 0); // verif data

    printf("OK\n");
}

void test_data_error_too_long()
{
    printf("Test: DATA trop long (>512)... ");
    uint8_t buffer[1024];
    uint8_t huge_data[513]; // 1 octet de trop

    int size = build_data(buffer, sizeof(buffer), 1, huge_data, 513);

    // doit échouer car TFTP limite les blocs à 512 octets
    assert(size == -1);
    printf("OK (Erreur détectée)\n");
}

void test_data_error_buffer_small()
{
    printf("Test: Buffer sortie trop petit... ");
    uint8_t buffer[10];
    uint8_t data[20];

    // 4 + 20 > 10
    int size = build_data(buffer, sizeof(buffer), 1, data, 20);

    assert(size == -1);
    printf("OK (Erreur détectée)\n");
}

void test_build_data()
{
    printf("\n=== TESTS BUILD_DATA ===\n");
    test_data_success_normal();
    test_data_max_size();
    test_data_error_too_long();
    test_data_error_buffer_small();
    printf("=== TOUS LES TESTS BUILD_DATA SONT PASSÉS ! ===\n");
}
// ----- build_ack -----
void test_ack_success()
{
    printf("Test: ACK valide (Block 1)... ");
    unsigned char buffer[10];
    uint16_t block = 1;

    int size = build_ack(buffer, sizeof(buffer), block);

    assert(size == 4); // taille d'un ack : 4

    assert(buffer[0] == 0 && buffer[1] == 4); // verif opcode

    assert(buffer[2] == 0 && buffer[3] == 1); // verif block

    printf("OK\n");
}

void test_ack_max_block()
{
    printf("Test: ACK Block Max (65535)... ");
    unsigned char buffer[10];
    uint16_t block = 65535; // 0xFFFF

    int size = build_ack(buffer, sizeof(buffer), block);

    assert(size == 4);
    // 0xFFFF reste FF FF peu importe l'endianness, mais on vérifie l'écriture
    assert(buffer[2] == 0xFF && buffer[3] == 0xFF);

    printf("OK\n");
}

void test_ack_buffer_too_small()
{
    printf("Test: Buffer trop petit... ");
    unsigned char buffer[3]; // < 4 octets

    int size = build_ack(buffer, sizeof(buffer), 1);

    assert(size == -1);
    printf("OK (Erreur détectée)\n");
}

void test_build_ack()
{
    printf("\n=== TESTS BUILD_ACK ===\n");
    test_ack_success();
    test_ack_max_block();
    test_ack_buffer_too_small();
    printf("=== TOUS LES TESTS BUILD_ACK SONT PASSÉS ! ===\n");
}

// --- build_error() -----
void test_error_success_standard()
{
    printf("Test: ERROR standard (File not found)... ");
    uint8_t buffer[100];
    const char *msg = "File not found";
    uint16_t err_code = 1;

    int size = build_error(buffer, sizeof(buffer), err_code, msg);

    // verif taille : 2(Op) + 2(Code) + 14(Msg) + 1(\0) = 19
    int expected_len = 2 + 2 + strlen(msg) + 1;
    assert(size == expected_len);

    assert(buffer[0] == 0 && buffer[1] == 5); // verif opcode

    assert(buffer[2] == 0 && buffer[3] == 1); // verif error code

    assert(strcmp((char *)(buffer + 4), msg) == 0); // verif msg

    printf("OK\n");
}

void test_error_empty_msg()
{
    printf("Test: ERROR message vide... ");
    uint8_t buffer[50];
    const char *msg = "";
    uint16_t err_code = 0; // 0 = "Not defined"

    int size = build_error(buffer, sizeof(buffer), err_code, msg);

    assert(size == 5); // 2 + 2 + 0 + 1 = 5

    // verif message vide
    assert(buffer[4] == 0);

    printf("OK\n");
}

void test_error_buffer_too_small()
{
    printf("Test: Buffer trop petit pour le message... ");
    uint8_t buffer[10];
    const char *msg = "Ce message est bien trop long pour ce minuscule buffer";

    int size = build_error(buffer, sizeof(buffer), 1, msg);

    assert(size == -1);
    printf("OK (Erreur détectée)\n");
}

void test_build_error()
{
    printf("\n=== TESTS BUILD_ERROR ===\n");
    test_error_success_standard();
    test_error_empty_msg();
    test_error_buffer_too_small();
    printf("=== TOUS LES TESTS BUILD_ERROR SONT PASSÉS ! ===\n");
}

// --- parse_opcode ---
void test_parse_opcode_rrq()
{
    printf("Test: Parse Opcode RRQ (1)... ");
    uint8_t buffer[2];

    // mock packet reseau
    buffer[0] = 0;
    buffer[1] = 1;

    uint16_t op;
    int res = parse_opcode(buffer, sizeof(buffer), &op);

    assert(res == 0);
    assert(op == 1); // vaut 1 après le ntohs
    printf("OK\n");
}

void test_parse_opcode_unknown()
{
    printf("Test: Parse Opcode Inconnu (99)... ");
    uint8_t buffer[2];

    buffer[0] = 0;
    buffer[1] = 99;

    uint16_t op;
    int res = parse_opcode(buffer, sizeof(buffer), &op);

    assert(res == 0); // parsing doit réussire
    assert(op == 99);
    printf("OK\n");
}

void test_parse_opcode_too_short()
{
    printf("Test: Buffer trop court (1 octet)... ");
    uint8_t buffer[1] = {0};

    uint16_t op = 0xFFFF;
    int res = parse_opcode(buffer, sizeof(buffer), &op);

    assert(res == -1);
    printf("OK (Erreur détectée)\n");
}

void test_parse_opcode()
{
    printf("\n=== TESTS PARSE_OPCODE ===\n");
    test_parse_opcode_rrq();
    test_parse_opcode_unknown();
    test_parse_opcode_too_short();
    printf("=== TOUS LES TESTS PARSE_OPCODE SONT PASSÉS ! ===\n");
}

// --- test_parse_block() ---

void test_parse_block_success()
{
    printf("Test: Parse Block standard (1)... ");
    uint8_t buffer[4];

    buffer[0] = 0;
    buffer[1] = 4; // mock un paquet ACK pour le bloc 1 : [00 04] [00 01]
    buffer[2] = 0;
    buffer[3] = 1; // Block Number

    uint16_t block;
    int res = parse_block(buffer, sizeof(buffer), &block);

    assert(res == 0);
    assert(block == 1);
    printf("OK\n");
}

void test_parse_block_too_short()
{
    printf("Test: Buffer trop court (< 4)... ");
    uint8_t buffer[3] = {0, 4, 0};
    uint16_t block = 123;
    int res = parse_block(buffer, sizeof(buffer), &block);

    assert(res == -1);
    printf("OK (Erreur détectée)\n");
}

void test_parse_block_max()
{
    printf("Test: Parse Block Max (65535)... ");
    uint8_t buffer[4];

    buffer[0] = 0;
    buffer[1] = 3; // opcode DATA
    buffer[2] = 0xFF;
    buffer[3] = 0xFF; // block 65535

    uint16_t block;
    int res = parse_block(buffer, sizeof(buffer), &block);

    assert(res == 0);
    assert(block == 65535);
    printf("OK\n");
}

void test_parse_block()
{
    printf("\n=== TESTS PARSE_BLOCK ===\n");
    test_parse_block_success();
    test_parse_block_too_short();
    test_parse_block_max();
    printf("=== TOUS LES TESTS PARSE_BLOCK SONT PASSÉS ! ===\n");
}

// --- test_parse_rrq_wrq ---
void test_parse_rrq_success()
{
    printf("Test: Parse RRQ valide (sans options)... ");
    // paquet : [0,1] "toto" \0 "octet" \0
    uint8_t buffer[] = {
        0, 1,                      // opcode RRQ
        't', 'o', 't', 'o', 0,     // filename "toto" + \0
        'o', 'c', 't', 'e', 't', 0 // mode "octet" + \0
    };

    char fname[100];
    char mode[100];
    int bigfile_req;
    uint16_t ws_req;

    int res = parse_rrq_wrq(buffer, sizeof(buffer), fname, sizeof(fname), mode, sizeof(mode), &bigfile_req, &ws_req);

    assert(res == 0);
    assert(strcmp(fname, "toto") == 0);
    assert(strcmp(mode, "octet") == 0);
    assert(bigfile_req == 0);
    assert(ws_req == 1);

    printf("OK\n");
}
void test_parse_rrq_with_options()
{
    printf("Test: Parse RRQ valide (avec options)... ");
    uint8_t buffer[] = {
        0, 1,
        't', 'o', 't', 'o', 0,
        'o', 'c', 't', 'e', 't', 0,
        'b', 'i', 'g', 'f', 'i', 'l', 'e', 0, '1', 0,
        'w', 'i', 'n', 'd', 'o', 'w', 's', 'i', 'z', 'e', 0, '8', 0};

    char fname[100];
    char mode[100];
    int bigfile_req;
    uint16_t ws_req;

    int res = parse_rrq_wrq(buffer, sizeof(buffer), fname, sizeof(fname), mode, sizeof(mode), &bigfile_req, &ws_req);

    assert(res == 0);
    assert(strcmp(fname, "toto") == 0);
    assert(bigfile_req == 1);
    assert(ws_req == 8);

    printf("OK\n");
}

void test_parse_rrq_output_too_small()
{
    printf("Test: Destination trop petite (fmax)... ");
    uint8_t buffer[] = {0, 1, 'l', 'o', 'n', 'g', 0, 'm', 0};

    char fname[3]; // trop petit pour "long" + \0
    char mode[100];
    int bigfile_req;
    uint16_t ws_req;

    int res = parse_rrq_wrq(buffer, sizeof(buffer), fname, sizeof(fname), mode, sizeof(mode), &bigfile_req, &ws_req);

    assert(res == -1);
    printf("OK (Erreur détectée)\n");
}

void test_parse_rrq_missing_null_filename()
{
    printf("Test: Paquet tronqué (Pas de 0 après filename)... ");
    uint8_t buffer[] = {0, 1, 'd', 'a', 't', 'a'}; // Paquet sans \0

    char fname[100];
    char mode[100];
    int bigfile_req;
    uint16_t ws_req;

    int res = parse_rrq_wrq(buffer, sizeof(buffer), fname, sizeof(fname), mode, sizeof(mode), &bigfile_req, &ws_req);

    assert(res == -1); // échoue car on atteint buffer_size sans trouver le 0
    printf("OK (Sécurité validée)\n");
}

void test_parse_rrq_missing_null_mode()
{
    printf("Test: Paquet tronqué (Pas de 0 après mode)... ");
    // filename OK, mais mode pas fini
    uint8_t buffer[] = {
        0, 1,
        'f', 0,       // filename "f"
        'o', 'c', 't' // mode tronqué
    };

    char fname[100];
    char mode[100];
    int bigfile_req;
    uint16_t ws_req;

    int res = parse_rrq_wrq(buffer, sizeof(buffer), fname, sizeof(fname), mode, sizeof(mode), &bigfile_req, &ws_req);

    assert(res == -1);
    printf("OK (Sécurité validée)\n");
}

void test_parse_rrq_wrq()
{
    printf("\n=== TESTS PARSE_RRQ_WRQ ===\n");
    test_parse_rrq_success();
    test_parse_rrq_with_options();
    test_parse_rrq_output_too_small();
    test_parse_rrq_missing_null_filename();
    test_parse_rrq_missing_null_mode();
    printf("=== TOUS LES TESTS PARSE_RRQ_WRQ SONT PASSÉS ! ===\n");
}
void test_parse_oack_success_both()
{
    printf("Test: Parse OACK valide (bigfile + windowsize)... ");
    uint8_t buffer[] = {
        0, 6,
        'b', 'i', 'g', 'f', 'i', 'l', 'e', 0, '1', 0,
        'w', 'i', 'n', 'd', 'o', 'w', 's', 'i', 'z', 'e', 0, '1', '6', 0};
    int bigfile_ack;
    uint16_t ws_ack;
    int res = parse_oack(buffer, sizeof(buffer), &bigfile_ack, &ws_ack);
    assert(res == 0);
    assert(bigfile_ack == 1);
    assert(ws_ack == 16);
    printf("OK\n");
}

void test_parse_oack_only_bigfile()
{
    printf("Test: Parse OACK valide (bigfile seul)... ");
    uint8_t buffer[] = {
        0, 6,
        'b', 'i', 'g', 'f', 'i', 'l', 'e', 0, '1', 0};
    int bigfile_ack;
    uint16_t ws_ack;
    int res = parse_oack(buffer, sizeof(buffer), &bigfile_ack, &ws_ack);
    assert(res == 0);
    assert(bigfile_ack == 1);
    assert(ws_ack == 1);
    printf("OK\n");
}

void test_parse_oack_only_windowsize()
{
    printf("Test: Parse OACK valide (windowsize seul)... ");
    uint8_t buffer[] = {
        0, 6,
        'w', 'i', 'n', 'd', 'o', 'w', 's', 'i', 'z', 'e', 0, '8', 0};
    int bigfile_ack;
    uint16_t ws_ack;
    int res = parse_oack(buffer, sizeof(buffer), &bigfile_ack, &ws_ack);
    assert(res == 0);
    assert(bigfile_ack == 0);
    assert(ws_ack == 8);
    printf("OK\n");
}

void test_parse_oack_unknown_options()
{
    printf("Test: Parse OACK avec options inconnues... ");
    uint8_t buffer[] = {
        0, 6,
        't', 's', 'i', 'z', 'e', 0, '0', 0,
        'b', 'i', 'g', 'f', 'i', 'l', 'e', 0, '1', 0};
    int bigfile_ack;
    uint16_t ws_ack;
    int res = parse_oack(buffer, sizeof(buffer), &bigfile_ack, &ws_ack);
    assert(res == 0);
    assert(bigfile_ack == 1);
    assert(ws_ack == 1);
    printf("OK\n");
}

void test_parse_oack_too_short()
{
    printf("Test: Parse OACK trop court... ");
    uint8_t buffer[] = {0};
    int bigfile_ack;
    uint16_t ws_ack;
    int res = parse_oack(buffer, sizeof(buffer), &bigfile_ack, &ws_ack);
    assert(res == -1);
    printf("OK\n");
}

void test_parse_oack_missing_null()
{
    printf("Test: Parse OACK tronqué (sans null final)... ");
    uint8_t buffer[] = {
        0, 6,
        'b', 'i', 'g', 'f', 'i', 'l', 'e', 0, '1'};
    int bigfile_ack;
    uint16_t ws_ack;
    int res = parse_oack(buffer, sizeof(buffer), &bigfile_ack, &ws_ack);
    assert(res == 0);
    assert(bigfile_ack == 0);
    printf("OK\n");
}

void test_parse_oack()
{
    printf("\n=== TESTS PARSE_OACK ===\n");
    test_parse_oack_success_both();
    test_parse_oack_only_bigfile();
    test_parse_oack_only_windowsize();
    test_parse_oack_unknown_options();
    test_parse_oack_too_short();
    test_parse_oack_missing_null();
    printf("=== TOUS LES TESTS PARSE_OACK SONT PASSÉS ! ===\n");
}

// ####### TEST SERVER ########

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
    printf("\nTest: Linked List (Create/Remove)... ");

    // Reset list
    client_list = NULL;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    // 1. Création de 3 clients et insertion manuelle en tête
    addr.sin_port = htons(1001);
    ClientContext *c1 = create_client(addr);
    c1->next = client_list;
    client_list = c1;

    addr.sin_port = htons(1002);
    ClientContext *c2 = create_client(addr);
    c2->next = client_list;
    client_list = c2;

    addr.sin_port = htons(1003);
    ClientContext *c3 = create_client(addr);
    c3->next = client_list;
    client_list = c3;

    // Vérif: L'insertion se fait en tête
    assert(client_list == c3);
    assert(client_list->next == c2);
    assert(client_list->next->next == c1);
}

void test_step_rrq()
{
    printf("Test: RRQ Logic (GET)... ");

    // Setup fichier source
    create_dummy_file("test_rrq.tmp", "HELLO_WORLD");

    // Setup Contexte
    ClientContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = socket(AF_INET, SOCK_DGRAM, 0); // Vrai socket pour éviter crash sendto
    ctx.state = STATE_RRQ;

    // Initialisations nécessaires pour passer l'assertion
    ctx.block = 1;
    ctx.block_32 = 1;
    ctx.window_size = 1;

    ctx.fp = fopen("test_rrq.tmp", "rb");
    assert(ctx.fp != NULL);

    // On simule que le bloc 1 (précédent) était un bloc PLEIN (512 octets data + 4 header).
    // Sinon, step_rrq pense que le transfert est fini et n'incrémente pas le bloc.
    ctx.last_packet_len = 516; // 4 + DATA_SIZE

    // Simulation Packet ACK 1 reçu (Le client confirme avoir reçu le bloc 1, on veut le 2)
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
    printf("Test: WRQ Logic (PUT - Normal)... ");

    // Setup
    ClientContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = socket(AF_INET, SOCK_DGRAM, 0);
    ctx.state = STATE_WRQ;
    ctx.block = 1; // On attend le bloc 1
    ctx.window_size = 1;
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
    assert(ctx.block == 1); // On attend maintenant le 2

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
    printf("Test: WRQ Logic (PUT - Duplicate)... ");

    ClientContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = socket(AF_INET, SOCK_DGRAM, 0);
    ctx.state = STATE_WRQ;
    ctx.block = 1; // On attend le bloc 2 (donc on a déjà reçu le 1)
    ctx.block_32 = 1;
    ctx.window_size = 1;

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
    assert(ctx.block == 1); // On attend TOUJOURS le 2, pas d'incrément

    // On doit avoir renvoyé l'ACK (test un peu aveugle ici sans mock sendto,
    // mais on vérifie que le code n'a pas planté et état stable)

    close(ctx.sock);
    printf("OK\n");
}

void test_handle_new_connection_rrq()
{
    printf("Test: Handle New Connection (RRQ)... ");

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
    assert(client_list->block_32 == 1);

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
    printf("=== LANCEMENT DES TESTS UNITAIRES ===\n");

    test_build_rrq_wrq();
    test_build_data();
    test_build_ack();
    test_build_error();
    test_build_oack();

    test_parse_rrq_wrq();
    test_parse_opcode();
    test_parse_block();
    test_parse_oack();

    test_linked_list_management();
    test_step_rrq();
    test_step_wrq_normal();
    test_step_wrq_duplicate();
    test_handle_new_connection_rrq();

    printf("=== TOUS LES TESTS SONT PASSÉS ===\n");

    return 0;
}