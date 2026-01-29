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
// retourne une chaine contenant les données du fichier filename
// et la taille de cette chaine via le pointeur data_size
char *load_file(char *filename, size_t *data_size)
{
    FILE *file = fopen(filename, "rb");
    size_t size;
    char *data;
    if (file == NULL)
    {
        fprintf(stderr, "Erreur: impossible de lire le fichier %s\n", filename);
        return NULL;
    }
    // calcule taille du fichier
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);

    data = (char *)malloc(size * sizeof(char));
    if (data == NULL)
    {
        fprintf(stderr, "Erreur: échec du malloc data\n");
        fclose(file);
        return NULL;
    }
    // on lit 1 octet size fois
    fread(data, 1, size, file);
    fclose(file);

    *data_size = size;
    return data;
}

void send_data(int sockfd, sockaddr_in *addr, unsigned char *data, size_t data_size)
{
    size_t offset = 0;
    unsigned int block_number = 1;
    socklen_t addr_len = sizeof(sockaddr_in);
    unsigned char packet[516]; // 4 (header) + 512 (data)
    int keep_sending = 1;      // booleen qui indique si on doit continuer d'envoyer
    size_t packet_len;

    // boucle des paquets
    while (offset < data_size || keep_sending)
    {
        size_t size_left = data_size - offset;
        size_t chunk_size = (size_left > DATA_SIZE) ? DATA_SIZE : size_left;

        packet_len = build_data(packet, sizeof(packet), block_number, data, data_size);

        // boucle de transmission
        unsigned short tries = 0;
        int ack_received = 0;

        while (tries < MAX_RETRIES && !ack_received)
        { // envoi paquet
            printf("Envoi bloc #%d (%zu octets)...\n", block_number, chunk_size);
            sendto(sockfd, packet, packet_len, 0, (struct sockaddr *)addr, addr_len);

            // attend ACK
            char buffer_ack[4];
            sockaddr_in from_addr;
            socklen_t from_len = sizeof(from_addr);

            int n = recvfrom(sockfd, buffer_ack, sizeof(buffer_ack), 0, (struct sockaddr *)&from_addr, &from_len);
            if (n < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    printf("Timeout... On renvoie !");
                    tries++;
                }
                else
                {
                    fprintf(stderr, "Erreur fatale recvfrom\n");
                    return;
                }
            }
            else if (n >= 4) // analyse du paquet reçu
            {
                unsigned short rcv_opcode = ntohs(*(unsigned short *)buffer_ack);
                unsigned short rcv_block = ntohs(*(unsigned short *)buffer_ack + 2);
                if (rcv_opcode == OPCODE_ACK && rcv_block == block_number)
                {
                    printf("ACK #%d reçu\n", rcv_block);
                    ack_received = 1;
                }
                else
                {
                    printf("Paquet ignoré (opcode: %d, bloc: %d)\n", rcv_opcode, rcv_block);
                }
            }
        }
        // connexion perdue
        if (!ack_received)
        {
            fprintf(stderr, "Erreur: abandon après %d essais pour le bloc %d\n", tries, block_number);
            return;
        }
        // préparer le prochain tour
        offset += chunk_size;
        block_number++;

        // fin du transfert
        if (chunk_size < DATA_SIZE)
        {
            keep_sending = 0;
        }
    }
    printf("Transfert terminé avec succès\n");
}
/*
int split_data(FILE *file, char *buffer)
{
    char c;
    int data_len = 0;
    while ((c = fgetc(file)) != EOF && data_len < 512)
    {
        buffer[data_len] = c;
    }
    // bzero(buffer);
    return data_len;
}
    */

int safe_name(const char *name)
{
    if (name[0] == '/')
        return 0;
    if (strstr(name, "..") != NULL)
        return 0;
    return 1;
}

/* --------------- Builders --------------- */

int build_rrq_wrq(uint16_t op_code, unsigned char *buffer, size_t buffer_size, const char *filename)
{
    if (op_code != OPCODE_RRQ && op_code != OPCODE_WRQ)
    {
        fprintf(stderr, "Erreur RRQ/WRQ: le code est ni RRQ ni WRQ\n");
        return -1;
    }
    size_t filename_len = strlen(filename);
    // opcode + len(filename) + \0 + len("octet") + \0
    if (2 + filename_len + 1 + 5 + 1 > buffer_size)
    {
        fprintf(stderr, "Erreur: nom du fichier trop long (%ld octets)\n", filename_len);
        return -1;
    }

    int offset = 0;
    u_int16_t opn = htons(op_code);
    memcpy(buffer + offset, &opn, 2);
    offset += 2;

    memcpy(buffer + offset, filename, filename_len + 1);
    offset += filename_len + 1;

    memcpy(buffer + offset, "octet", 6);
    offset += strlen("octet") + 1;

    return offset;
}
int build_data(uint8_t *buffer, size_t buffer_size, uint16_t block_number,
               const uint8_t *data, size_t data_len)
{
    if (data_len > DATA_SIZE)
        return -1;
    if (buffer_size < 4 + data_len)
        return -1;

    uint16_t opn = htons(OPCODE_DATA); // opcode pour le reseau (big vs little endian)
    uint16_t bn = htons(block_number); // pareil pour le numero de block
    memcpy(buffer, &opn, 2);
    memcpy(buffer + 2, &bn, 2);
    memcpy(buffer + 4, data, data_len);
    return (int)(4 + data_len);
}
int build_ack(unsigned char *buffer, size_t buffer_size, uint16_t block_number)
{
    if (buffer_size < 4)
        return -1;
    uint16_t opn = htons(OPCODE_ACK);
    uint16_t bn = htons(block_number);
    memcpy(buffer, &opn, 2);
    memcpy(buffer + 2, &bn, 2);
    return 4;
}

int build_error(uint8_t *buffer, size_t buffer_size, uint16_t error_code, const char *error_msg)
{
    size_t msg_len = strlen(error_msg);
    size_t need = 2 + 2 + msg_len + 1; // opcode + errorcode + msg + \0
    if (need > buffer_size)
        return -1;

    uint16_t opn = htons(OPCODE_ERROR);
    uint16_t cn = htons(error_code);
    memcpy(buffer, &opn, 2);
    memcpy(buffer + 2, &cn, 2);
    memcpy(buffer + 4, error_msg, msg_len);
    buffer[4 + msg_len] = 0;
    return (int)need;
}

/* --------------- Parsers --------------- */

int parse_opcode(const uint8_t *buffer, size_t buffer_size, uint16_t *opcode)
{
    if (buffer_size < 2)
        return -1;
    uint16_t x;
    memcpy(&x, buffer, 2);
    *opcode = ntohs(x);
    return 0;
}

int parse_block(const uint8_t *buffer, size_t buffer_size, uint16_t *block_number)
{
    if (buffer_size < 4)
        return -1;
    uint16_t x;
    memcpy(&x, buffer + 2, 2);
    *block_number = ntohs(x);
    return 0;
}

// RRQ/WRQ: [op(2)] [filename]\0 [mode]\0
int parse_rrq_wrq(const uint8_t *buffer, size_t buffer_size,
                  char *filename, size_t fmax,
                  char *mode, size_t mmax)
{
    if (buffer_size < 4)
        return -1;
    size_t i = 2;

    size_t f = 0;
    while (i < buffer_size && buffer[i] != 0)
    {
        if (f + 1 >= fmax)
            return -1;
        filename[f++] = (char)buffer[i++];
    }
    if (i >= buffer_size || buffer[i] != 0)
        return -1;
    filename[f] = 0;
    i++;

    size_t m = 0;
    while (i < buffer_size && buffer[i] != 0)
    {
        if (m + 1 >= mmax)
            return -1;
        mode[m++] = (char)buffer[i++];
    }
    if (i >= buffer_size || buffer[i] != 0)
        return -1;
    mode[m] = 0;

    return 0;
}