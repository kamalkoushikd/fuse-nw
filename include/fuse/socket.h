#ifndef FUSE_SOCKET_H
#define FUSE_SOCKET_H

#include <stddef.h>
#include <stdint.h>

#include "fuse/export.h"
#include "fuse/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Thin wrapper around a Linux UDP socket. Fuse runs its own state
 * machine on top of datagrams rather than delegating to the kernel's
 * connected-UDP or TCP-like semantics, so callers own the event loop
 * (poll/epoll/io_uring) and only use this for the socket itself. */
typedef struct fuse_socket fuse_socket;

/* Opens and binds an IPv4 UDP socket to local_addr:local_port.
 * local_addr may be NULL or "0.0.0.0" to bind all interfaces;
 * local_port may be 0 to let the kernel choose an ephemeral port.
 * Returns NULL on failure. */
FUSE_EXPORT fuse_socket *fuse_socket_open(const char *local_addr, uint16_t local_port);

FUSE_EXPORT void fuse_socket_close(fuse_socket *sock);

/* Returns the underlying file descriptor, e.g. to register with the
 * caller's own poll/epoll event loop. */
FUSE_EXPORT int fuse_socket_fd(const fuse_socket *sock);

FUSE_EXPORT fuse_status fuse_socket_set_nonblocking(fuse_socket *sock, int nonblocking);

FUSE_EXPORT fuse_status fuse_socket_send_to(fuse_socket *sock,
                                             const uint8_t *data, size_t len,
                                             const char *dst_addr, uint16_t dst_port);

/* Receives at most one datagram into `buf`. On success, *out_len holds
 * the number of bytes received and, if src_addr/src_port are non-NULL,
 * they describe the sender. Returns FUSE_ERR_SOCKET on error; for a
 * non-blocking socket with no datagram pending, errno is left as
 * EAGAIN/EWOULDBLOCK for the caller to check. */
FUSE_EXPORT fuse_status fuse_socket_recv_from(fuse_socket *sock,
                                               uint8_t *buf, size_t buf_len, size_t *out_len,
                                               char *src_addr, size_t src_addr_len,
                                               uint16_t *src_port);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_SOCKET_H */
