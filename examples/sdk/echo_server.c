/* Fuse SDK example: a concurrent echo server in C.
 *
 *   cc echo_server.c -o echo_server $(pkg-config --cflags --libs fuse)
 *   ./echo_server 4433 [pre-shared-key]
 *
 * Accepts connections forever and handles each on its own thread, which is
 * the same shape you would write for a TCP server. */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fuse/sdk.h>

static void *serve(void *arg) {
    fuse_conn *conn = (fuse_conn *)arg;

    char addr[64] = "?";
    uint16_t port = 0;
    fuse_conn_peer(conn, addr, sizeof(addr), &port);
    printf("[+] %s:%u connected\n", addr, port);

    for (;;) {
        void *msg = NULL;
        size_t len = 0;
        /* recv_alloc sizes the buffer for us, so messages of any length
         * work without guessing. */
        fuse_status st = fuse_recv_alloc(conn, &msg, &len, 30000);
        if (st != FUSE_OK) {
            printf("[-] %s:%u %s\n", addr, port, fuse_strerror(st));
            break;
        }
        printf("[>] %s:%u %zu bytes\n", addr, port, len);

        st = fuse_send(conn, msg, len); /* echo it straight back */
        fuse_free(msg);
        if (st != FUSE_OK) {
            printf("[-] %s:%u send: %s\n", addr, port, fuse_strerror(st));
            break;
        }
    }

    fuse_close(conn);
    return NULL;
}

int main(int argc, char **argv) {
    /* Line-buffer stdout so a long-running server's log appears live even
     * when piped to a file or a container log collector. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 2) {
        fprintf(stderr, "usage: %s <port> [pre-shared-key]\n", argv[0]);
        return 2;
    }

    fuse_config cfg;
    fuse_config_init(&cfg);
    cfg.bind_address = "0.0.0.0";
    cfg.port = (uint16_t)atoi(argv[1]);
    cfg.pre_shared_key = (argc > 2) ? argv[2] : NULL;

    fuse_status err = FUSE_OK;
    fuse_listener *l = fuse_listen(&cfg, &err);
    if (!l) {
        fprintf(stderr, "listen: %s\n", fuse_strerror(err));
        return 1;
    }
    printf("echo server on port %u%s (fuse %s)\n", fuse_listener_port(l),
           cfg.pre_shared_key ? ", encrypted" : "", fuse_version());

    for (;;) {
        fuse_conn *conn = fuse_accept(l, -1 /* wait indefinitely */, &err);
        if (!conn) {
            fprintf(stderr, "accept: %s\n", fuse_strerror(err));
            continue;
        }
        pthread_t t;
        if (pthread_create(&t, NULL, serve, conn) != 0) {
            fuse_close(conn);
            continue;
        }
        pthread_detach(t);
    }
}
