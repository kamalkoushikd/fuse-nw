/* Fuse SDK example: the matching echo client.
 *
 *   cc echo_client.c -o echo_client $(pkg-config --cflags --libs fuse)
 *   ./echo_client 127.0.0.1 4433 "hello" [pre-shared-key] */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fuse/sdk.h>

int main(int argc, char **argv) {
    /* Line-buffer stdout so a long-running server's log appears live even
     * when piped to a file or a container log collector. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 4) {
        fprintf(stderr, "usage: %s <host> <port> <message> [pre-shared-key]\n", argv[0]);
        return 2;
    }

    fuse_config cfg;
    fuse_config_init(&cfg);
    cfg.host = argv[1];
    cfg.port = (uint16_t)atoi(argv[2]);
    cfg.pre_shared_key = (argc > 4) ? argv[4] : NULL;
    cfg.timeout_ms = 5000;

    fuse_status err = FUSE_OK;
    fuse_conn *conn = fuse_connect(&cfg, &err);
    if (!conn) {
        fprintf(stderr, "connect: %s\n", fuse_strerror(err));
        return 1;
    }

    const char *msg = argv[3];
    if (fuse_send(conn, msg, strlen(msg)) != FUSE_OK) {
        fprintf(stderr, "send failed\n");
        fuse_close(conn);
        return 1;
    }

    char buf[65536];
    size_t n = 0;
    fuse_status st = fuse_recv(conn, buf, sizeof(buf), &n, 5000);
    if (st != FUSE_OK) {
        fprintf(stderr, "recv: %s\n", fuse_strerror(st));
        fuse_close(conn);
        return 1;
    }
    printf("echo: %.*s\n", (int)n, buf);

    fuse_conn_stats s;
    fuse_conn_get_stats(conn, &s);
    printf("(%llu sent, %llu received, rtt %llu us)\n",
           (unsigned long long)s.messages_sent,
           (unsigned long long)s.messages_received,
           (unsigned long long)s.rtt_us);

    fuse_close(conn);
    return 0;
}
