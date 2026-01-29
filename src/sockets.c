#include "sockets.h"

void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

int addr_equal(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_family == b->sin_family &&
           a->sin_addr.s_addr == b->sin_addr.s_addr &&
           a->sin_port == b->sin_port;
}

ssize_t recvfrom_timeout(int sock, uint8_t *buf, size_t max,
                         struct sockaddr_in *src, int timeout_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int r = select(sock + 1, &rfds, NULL, NULL, &tv); // y a d'autres alternative comme SO_RCVTIMEO() mais select() c'est le plus fiable pour attendre X ms
    if (r < 0)
        return -1;
    if (r == 0)
        return 0; // timeout

    socklen_t sl = sizeof(*src);
    return recvfrom(sock, buf, max, 0, (struct sockaddr *)src, &sl); // te dit qui t’a répondu (IP+port)
}