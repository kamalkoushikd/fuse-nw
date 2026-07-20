/* Minimal UDP echo server built on fuse's socket wrapper (fuse/socket.h),
 * demonstrating the library's plain-C API. It has nothing to do with
 * fuse's own wire protocol yet - see docs/ROADMAP.md for that - it just
 * exercises fuse_socket_open/recv_from/send_to. */

#include <stdio.h>
#include <stdlib.h>

#include <fuse/fuse.h>

int main(int argc, char **argv) {
    uint16_t port = 9999;
    if (argc > 1) {
        port = (uint16_t)atoi(argv[1]);
    }

    fuse_socket *sock = fuse_socket_open("127.0.0.1", port);
    if (!sock) {
        fprintf(stderr, "failed to open socket on 127.0.0.1:%u\n", port);
        return 1;
    }

    printf("fuse_echo_server listening on 127.0.0.1:%u (Ctrl+C to stop)\n", port);

    uint8_t buf[2048];
    for (;;) {
        size_t received = 0;
        char src_addr[64];
        uint16_t src_port = 0;

        fuse_status status = fuse_socket_recv_from(sock, buf, sizeof(buf), &received, src_addr,
                                                    sizeof(src_addr), &src_port);
        if (status != FUSE_OK) {
            fprintf(stderr, "recv failed\n");
            break;
        }

        printf("received %zu bytes from %s:%u\n", received, src_addr, src_port);

        status = fuse_socket_send_to(sock, buf, received, src_addr, src_port);
        if (status != FUSE_OK) {
            fprintf(stderr, "send failed\n");
            break;
        }
    }

    fuse_socket_close(sock);
    return 0;
}
