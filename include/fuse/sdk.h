#ifndef FUSE_SDK_H
#define FUSE_SDK_H

/*
 * Fuse SDK — a socket-style API for the Fuse transport.
 *
 * If you have written a TCP server or client, this will look familiar:
 *
 *      server                          client
 *      ------                          ------
 *      l = fuse_listen(cfg)            c = fuse_connect(cfg)
 *      c = fuse_accept(l)              fuse_send(c, buf, len)
 *      fuse_recv(c, buf, cap, &n)      fuse_recv(c, buf, cap, &n)
 *      fuse_send(c, buf, len)          fuse_close(c)
 *      fuse_close(c)
 *      fuse_listener_close(l)
 *
 * Differences from a TCP socket worth knowing before you write code:
 *
 *   * MESSAGE ORIENTED, not a byte stream. What one fuse_send() delivers,
 *     exactly one fuse_recv() returns — boundaries are preserved, like
 *     SOCK_SEQPACKET. You never have to length-prefix or scan for
 *     delimiters, and a partial read cannot happen.
 *   * Reliable and ordered, over UDP. Loss is repaired by retransmission
 *     underneath; you see either the whole message or an error.
 *   * fuse_send() returns once the peer has acknowledged the message, not
 *     when it has merely been queued locally. So a successful send means
 *     it arrived.
 *   * Optional built-in encryption: set a pre-shared key on both ends.
 *
 * This is a C API on purpose: it is the stable ABI that the Python
 * bindings (and any other language) attach to. C++ callers may prefer
 * <fuse/transfer.hpp> for whole-file/buffer transfers.
 *
 * THREAD SAFETY. A fuse_conn may be used from multiple threads, but a
 * given connection serialises its own send and receive internally. The
 * common pattern of one thread sending and another receiving on the same
 * connection is supported. A fuse_listener may only be accepted from one
 * thread at a time.
 *
 * Platform: Linux (uses recvmmsg and UDP segmentation offload).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Status codes ---------------------------------------------------- */

typedef enum fuse_status {
    FUSE_OK = 0,
    FUSE_ERR_CONFIG = -1,      /* nonsensical configuration               */
    FUSE_ERR_SOCKET = -2,      /* bind/socket failure; port in use?       */
    FUSE_ERR_TIMEOUT = -3,     /* no peer, or peer went away              */
    FUSE_ERR_CLOSED = -4,      /* the peer closed the connection          */
    FUSE_ERR_AUTH = -5,        /* wrong pre-shared key, or tampering      */
    FUSE_ERR_TOO_LARGE = -6,   /* message exceeds fuse_max_message()      */
    FUSE_ERR_BUFFER = -7,      /* caller buffer too small; see *out_len   */
    FUSE_ERR_UNSUPPORTED = -8, /* built without the crypto backend        */
    FUSE_ERR_INTERNAL = -9
} fuse_status;

/* Human-readable form of a status code. Never returns NULL. */
const char *fuse_strerror(fuse_status status);

/* Library version string, e.g. "0.1.0". */
const char *fuse_version(void);

/* Non-zero if this build can encrypt. When zero, a config carrying a
 * pre_shared_key is refused with FUSE_ERR_UNSUPPORTED rather than being
 * silently downgraded to plaintext. */
int fuse_encryption_available(void);

/* Largest message a single send/recv can carry, in bytes. */
size_t fuse_max_message(void);

/* ---- Configuration --------------------------------------------------- */

typedef struct fuse_config {
    /* Server: address to bind, e.g. "0.0.0.0" (NULL means all interfaces).
     * Client: ignored. */
    const char *bind_address;

    /* Client: server hostname or dotted-quad address. Server: ignored. */
    const char *host;

    /* Server: the port to listen on. Client: the server's listen port.
     * Unlike the file-transfer API, only this ONE port needs to be open;
     * each accepted connection then moves to its own ephemeral port,
     * exactly as a TFTP server does. */
    uint16_t port;

    /* NULL or "" for plaintext. Otherwise both ends must set the same key
     * and every message is sealed with AES-256-GCM.
     *
     * NOTE: this provides confidentiality and integrity under a key that is
     * fresh per session, but NOT forward secrecy — an attacker who records
     * traffic and later learns this key can decrypt it. */
    const char *pre_shared_key;

    /* How long an operation waits before giving up. 0 selects the default
     * (10s). Individual calls can override with their own timeout. */
    uint32_t timeout_ms;
} fuse_config;

/* Fills cfg with defaults. Always call this first, then override fields —
 * it keeps your code source-compatible if fields are added later. */
void fuse_config_init(fuse_config *cfg);

/* ---- Handles --------------------------------------------------------- */

typedef struct fuse_listener fuse_listener;
typedef struct fuse_conn fuse_conn;

/* ---- Server ---------------------------------------------------------- */

/* Binds cfg->port and returns a listener, or NULL on failure (the reason
 * is written to *err if err is non-NULL). */
fuse_listener *fuse_listen(const fuse_config *cfg, fuse_status *err);

/* Waits for an incoming connection and returns it.
 *
 * timeout_ms: <0 waits indefinitely, 0 polls, >0 waits that long and then
 * returns NULL with *err == FUSE_ERR_TIMEOUT.
 *
 * The returned connection is independent of the listener: you may keep
 * accepting while previously accepted connections are still in use, so the
 * usual accept-loop-and-hand-off-to-a-thread pattern works. */
fuse_conn *fuse_accept(fuse_listener *l, int timeout_ms, fuse_status *err);

/* The port the listener is actually bound to. Useful when cfg->port was 0
 * and the kernel chose one. */
uint16_t fuse_listener_port(const fuse_listener *l);

void fuse_listener_close(fuse_listener *l);

/* ---- Client ---------------------------------------------------------- */

/* Connects to cfg->host:cfg->port. Returns NULL on failure with *err set;
 * FUSE_ERR_TIMEOUT means nothing answered, FUSE_ERR_AUTH means the peer
 * did not prove possession of the same pre-shared key. */
fuse_conn *fuse_connect(const fuse_config *cfg, fuse_status *err);

/* ---- Data ------------------------------------------------------------ */

/* Sends one message. Returns FUSE_OK once the peer has acknowledged it.
 * A zero-length message is legal and is delivered as such. */
fuse_status fuse_send(fuse_conn *c, const void *data, size_t len);

/* Receives one whole message into buf.
 *
 * On FUSE_OK, *out_len holds the message length. If the message does not
 * fit, returns FUSE_ERR_BUFFER and sets *out_len to the size required —
 * the message is NOT discarded, so you can allocate and call again.
 *
 * timeout_ms: <0 waits indefinitely, 0 polls, >0 bounds the wait.
 * Returns FUSE_ERR_CLOSED once the peer has closed and no messages
 * remain. */
fuse_status fuse_recv(fuse_conn *c, void *buf, size_t cap, size_t *out_len,
                      int timeout_ms);

/* Same, but allocates. On FUSE_OK the caller owns *out and must release it
 * with fuse_free(). Convenient when message sizes vary. */
fuse_status fuse_recv_alloc(fuse_conn *c, void **out, size_t *out_len,
                            int timeout_ms);

/* Releases memory handed out by fuse_recv_alloc(). */
void fuse_free(void *p);

/* ---- Connection state ------------------------------------------------ */

/* Writes the peer's address into addr (dotted-quad) and port. Either
 * pointer may be NULL. Handy for logging in an accept loop. */
fuse_status fuse_conn_peer(const fuse_conn *c, char *addr, size_t addr_cap,
                           uint16_t *port);

/* Non-zero once the peer has closed or the connection has failed. */
int fuse_conn_is_closed(const fuse_conn *c);

typedef struct fuse_conn_stats {
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t bytes_sent;      /* application payload, not wire bytes */
    uint64_t bytes_received;
    uint64_t retransmits;     /* blocks this side had to resend      */
    uint64_t auth_failures;   /* blocks rejected by AEAD             */
    uint64_t rtt_us;          /* smoothed round-trip estimate        */
} fuse_conn_stats;

void fuse_conn_get_stats(const fuse_conn *c, fuse_conn_stats *out);

/* Sends a close notification to the peer (so its next recv returns
 * FUSE_ERR_CLOSED promptly rather than timing out) and frees the
 * connection. Passing NULL is a no-op. */
void fuse_close(fuse_conn *c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FUSE_SDK_H */
