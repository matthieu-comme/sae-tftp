#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>
#include "tftp_utils.h"

// pour afficher le buffer en cas d'erreur
void print_hex(char *buffer, int size)
{
    for (int i = 0; i < size; i++)
    {
        printf("%02x ", (unsigned char)buffer[i]);
    }
    printf("\n");
}

void test_rrq_success()
{
    printf("Test: RRQ valide... ");
    unsigned char buffer[DATA_SIZE];
    char *fname = "test_rrq.txt";
    int size = build_rrq_wrq(OPCODE_RRQ, buffer, sizeof(buffer), fname);
    printf("size : %d", size);
    assert(size == 2 + ((int)strlen(fname) + 1) + 5 + 1);                   // Opcode + filename\0 + octet\0
    assert(strcmp((char *)(buffer + 2), fname) == 0);                       // filename
    assert(strcmp((char *)(buffer + 2 + strlen(fname) + 1), "octet") == 0); // Mode correct
    printf("OK\n");
}
// ----- build_rrq_wrq() -----
void test_wrq_success()
{
    printf("Test: WRQ valide... ");
    unsigned char buffer[DATA_SIZE];
    char *fname = "test_wrq.txt";
    int size = build_rrq_wrq(OPCODE_WRQ, buffer, sizeof(buffer), fname);

    assert(size == 2 + ((int)strlen(fname) + 1) + 6);
    assert(strcmp((char *)(buffer + 2), fname) == 0);
    assert(strcmp((char *)(buffer + 2 + strlen(fname) + 1), "octet") == 0);
    printf("OK\n");
}

void test_invalid_opcode()
{
    printf("Test: Opcode invalide (99)... ");
    unsigned char buffer[DATA_SIZE];
    int size = build_rrq_wrq(99, buffer, sizeof(buffer), "file.txt");
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

    int size = build_rrq_wrq(OPCODE_RRQ, buffer, sizeof(buffer), long_name);
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
    test_invalid_opcode();
    test_filename_limit();
    printf("=== TOUS LES TESTS BUILD_RRQ_WRQ SONT PASSÉS ! ===\n");
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
    printf("Test: Parse RRQ valide... ");
    // paquet : [0,1] "toto" \0 "octet" \0
    uint8_t buffer[] = {
        0, 1,                      // opcode RRQ
        't', 'o', 't', 'o', 0,     // filename "toto" + \0
        'o', 'c', 't', 'e', 't', 0 // mode "octet" + \0
    };

    char fname[100];
    char mode[100];

    int res = parse_rrq_wrq(buffer, sizeof(buffer), fname, sizeof(fname), mode, sizeof(mode));

    assert(res == 0);
    assert(strcmp(fname, "toto") == 0);
    assert(strcmp(mode, "octet") == 0);

    printf("OK\n");
}

void test_parse_rrq_output_too_small()
{
    printf("Test: Destination trop petite (fmax)... ");
    uint8_t buffer[] = {0, 1, 'l', 'o', 'n', 'g', 0, 'm', 0};

    char fname[3]; // trop petit pour "long" + \0
    char mode[100];

    int res = parse_rrq_wrq(buffer, sizeof(buffer), fname, sizeof(fname), mode, sizeof(mode));

    assert(res == -1);
    printf("OK (Erreur détectée)\n");
}

void test_parse_rrq_missing_null_filename()
{
    printf("Test: Paquet tronqué (Pas de 0 après filename)... ");
    uint8_t buffer[] = {0, 1, 'd', 'a', 't', 'a'}; // Paquet sans \0

    char fname[100];
    char mode[100];

    int res = parse_rrq_wrq(buffer, sizeof(buffer), fname, sizeof(fname), mode, sizeof(mode));

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

    int res = parse_rrq_wrq(buffer, sizeof(buffer), fname, sizeof(fname), mode, sizeof(mode));

    assert(res == -1);
    printf("OK (Sécurité validée)\n");
}

void test_parse_rrq_wrq()
{
    printf("\n=== TESTS PARSE_RRQ_WRQ ===\n");
    test_parse_rrq_success();
    test_parse_rrq_output_too_small();
    test_parse_rrq_missing_null_filename();
    test_parse_rrq_missing_null_mode();
    printf("=== TOUS LES TESTS PARSE_RRQ_WRQ SONT PASSÉS ! ===\n");
}
int main()
{
    test_build_rrq_wrq();
    test_build_data();
    test_build_ack();
    test_build_error();

    test_parse_opcode();
    test_parse_block();
    test_parse_rrq_wrq();

    return 0;
}