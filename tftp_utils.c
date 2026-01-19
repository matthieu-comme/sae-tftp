#include "tftp_utils.h"

void display_packet(const char *buffer, int size)
{
    printf("\n--- Contenu du paquet TFTP (%d octets) ---\n", size);

    // opcode
    uint16_t opcode;
    memcpy(&opcode, buffer, 2);
    printf("Opcode : %d (Network order: 0x%04x)\n", ntohs(opcode), opcode);

    // affichage hexa
    printf("Raw Hex: ");
    for (int i = 0; i < size; i++)
    {
        printf("%02x ", (unsigned char)buffer[i]);
    }

    // affichage lisible
    printf("\nString : ");
    for (int i = 0; i < size; i++)
    {
        if (buffer[i] == '\0')
        {
            printf("|"); // séparateur fin de chaine
        }
        else if (isprint(buffer[i]))
        {
            printf("%c", buffer[i]);
        }
        else
        {
            printf("."); // char non imprimable (comme opcode)
        }
    }
    printf("\n------------------------------------------\n\n");
}

int build_rrq_wrq(int op_code, char *buffer, char *filename)
{
    if (op_code != RRQ && op_code != WRQ)
    {
        fprintf(stderr, "Erreur RRQ/WRQ: le code est ni RRQ ni WRQ\n");
        return -1;
    }
    int filename_len = strlen(filename) + 1;
    if (filename_len + 8 > BUFFER_SIZE)
    {
        printf("Erreur: nom du fichier trop long (%d octets)\n", filename_len);
        return -1;
    }

    int offset = 0;
    memcpy(buffer + offset, &op_code, 2);
    offset += 2;

    strcpy(buffer + offset, filename);
    offset += filename_len;

    strcpy(buffer + offset, "octet");
    offset += strlen("octet") + 1;

    return offset;
}

// config de l'adresse du serveur
int init_server_addr(sockaddr_in *server_addr)
{
    memset(server_addr, 0, sizeof(sockaddr_in));
    server_addr->sin_family = AF_INET;
    server_addr->sin_port = htons(SERVER_PORT);
    server_addr->sin_addr.s_addr = inet_addr("127.0.0.1");

    printf("Socket créée et adresse configurée.\n");
    return 0;
}

int split_data()
{
}