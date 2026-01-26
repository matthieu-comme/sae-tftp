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
    if (op_code != OPCODE_RRQ && op_code != OPCODE_WRQ)
    {
        fprintf(stderr, "Erreur RRQ/WRQ: le code est ni RRQ ni WRQ\n");
        return -1;
    }
    int filename_len = strlen(filename) + 1;
    if (filename_len + 8 > DATA_SIZE)
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

void send_data(int sockfd, sockaddr_in *addr, char *data, size_t data_size)
{
    size_t offset = 0;
    unsigned int block_number = 1;
    socklen_t addr_len = sizeof(sockaddr_in);
    char packet[516];          // 4 (header) + 512 (data)
    int keep_sending = 1;      // booleen qui indique si on doit continuer d'envoyer
    unsigned short opcode_net; // opcode pour le reseau (big vs little endian)
    unsigned short block_net;  // pareil pour le numero de block
    size_t packet_len;

    // boucle des paquets
    while (offset < data_size || keep_sending)
    {
        size_t size_left = data_size - offset;
        size_t chunk_size = (size_left > DATA_SIZE) ? DATA_SIZE : size_left;

        // construction du header du paquet
        opcode_net = htons(OPCODE_DATA);
        block_net = htons(block_number);

        memcpy(packet, &opcode_net, 2);
        memcpy(packet + 2, &block_net, 2);

        if (chunk_size > 0)
        {
            memcpy(packet + 4, data + offset, chunk_size); // cpy data dans packet
        }
        packet_len = 4 + chunk_size;

        // boucle de transmission
        unsigned short tries = 0;
        int ack_received = 0;

        while (tries < MAX_RETRIES && !ack_received)
        { // envoi paquet
            printf("Envoi bloc #%d (%zu octets)...\n");
            sendto(sockfd, packet, packet_len, 0, addr, addr_len);

            // attend ACK
            char buffer_ack[4];
            sockaddr_in from_addr;
            socklen_t from_len = sizeof(from_addr);

            int n = recvfrom(sockfd, buffer_ack, sizeof(buffer_ack), 0, &from_addr, &from_len);
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