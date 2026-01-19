#include "tftp_utils.h"

int main(int argc, char **argv)
{
    int sockfd;
    sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];

    // creation socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        fprintf(stderr, "Erreur creation socket\n");
        return -1;
    }
    init_server_addr(&server_addr);

    int packet_size = build_rrq_wrq(WRQ, buffer, "fichier.txt");
    if (packet_size > 0)
    {
        printf("Requete RRQ envoyee (%d octets)\n", packet_size);
        display_packet(buffer, packet_size);
    }
    else
        printf("Non envoyé. Taille du paquet : %d octets\n", packet_size);
    // sendto(sockfd, )
    close(sockfd);
    return 0;
}