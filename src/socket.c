#include "fuse/socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct fuse_socket {
    int fd;
};

fuse_socket *fuse_socket_open(const char *local_addr, uint16_t local_port) {
    fuse_socket *sock = malloc(sizeof(*sock));
    if (!sock) {
        return NULL;
    }

    sock->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock->fd < 0) {
        free(sock);
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(local_port);

    if (!local_addr || strcmp(local_addr, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, local_addr, &addr.sin_addr) != 1) {
        close(sock->fd);
        free(sock);
        return NULL;
    }

    if (bind(sock->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock->fd);
        free(sock);
        return NULL;
    }

    return sock;
}

void fuse_socket_close(fuse_socket *sock) {
    if (!sock) {
        return;
    }
    close(sock->fd);
    free(sock);
}

int fuse_socket_fd(const fuse_socket *sock) {
    return sock ? sock->fd : -1;
}

fuse_status fuse_socket_set_nonblocking(fuse_socket *sock, int nonblocking) {
    if (!sock) {
        return FUSE_ERR_INVALID_ARGUMENT;
    }

    int flags = fcntl(sock->fd, F_GETFL, 0);
    if (flags < 0) {
        return FUSE_ERR_SOCKET;
    }

    flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (fcntl(sock->fd, F_SETFL, flags) != 0) {
        return FUSE_ERR_SOCKET;
    }

    return FUSE_OK;
}

fuse_status fuse_socket_send_to(fuse_socket *sock, const uint8_t *data, size_t len,
                                 const char *dst_addr, uint16_t dst_port) {
    if (!sock || !data || !dst_addr) {
        return FUSE_ERR_INVALID_ARGUMENT;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dst_port);
    if (inet_pton(AF_INET, dst_addr, &addr.sin_addr) != 1) {
        return FUSE_ERR_INVALID_ARGUMENT;
    }

    ssize_t sent = sendto(sock->fd, data, len, 0, (struct sockaddr *)&addr, sizeof(addr));
    if (sent < 0 || (size_t)sent != len) {
        return FUSE_ERR_SOCKET;
    }

    return FUSE_OK;
}

fuse_status fuse_socket_recv_from(fuse_socket *sock, uint8_t *buf, size_t buf_len, size_t *out_len,
                                   char *src_addr, size_t src_addr_len, uint16_t *src_port) {
    if (!sock || !buf || !out_len) {
        return FUSE_ERR_INVALID_ARGUMENT;
    }

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    ssize_t received = recvfrom(sock->fd, buf, buf_len, 0, (struct sockaddr *)&addr, &addr_len);
    if (received < 0) {
        return FUSE_ERR_SOCKET;
    }
    *out_len = (size_t)received;

    if (src_addr && src_addr_len > 0) {
        if (!inet_ntop(AF_INET, &addr.sin_addr, src_addr, (socklen_t)src_addr_len)) {
            return FUSE_ERR_SOCKET;
        }
    }
    if (src_port) {
        *src_port = ntohs(addr.sin_port);
    }

    return FUSE_OK;
}
