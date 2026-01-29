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

void test_load_file()
{
    size_t size;
    char *data = load_file("toto.txt", &size);
    printf("Longueur du fichier : %ld\n", size);
    printf("%s\n", data);
}

void test_build_rrq_wrq()
{
    printf("=== TESTS BUILD_RRQ_WRQ ===\n");
    test_rrq_success();
    test_wrq_success();
    test_invalid_opcode();
    test_filename_limit();
    printf("=== TOUS LES TESTS SONT PASSÉS ! ===\n");
}
int main()
{
    test_build_rrq_wrq();
    test_load_file();
    return 0;
}