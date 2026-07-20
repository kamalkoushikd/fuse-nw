#ifndef FUSE_CONNECTION_H
#define FUSE_CONNECTION_H

#include <stdint.h>

#include "fuse/export.h"
#include "fuse/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* fuse_connection ties together a socket, connection IDs, and (once
 * implemented) crypto/stream state for one endpoint of a connection.
 * The state machine below is intentionally minimal: it exists so the
 * rest of the library has a concrete type to build against while the
 * handshake, loss detection, and congestion control are implemented.
 * See docs/ROADMAP.md for the intended path to a full connection. */
typedef enum fuse_connection_state {
    FUSE_CONN_IDLE,
    FUSE_CONN_HANDSHAKING,
    FUSE_CONN_ESTABLISHED,
    FUSE_CONN_CLOSING,
    FUSE_CONN_CLOSED
} fuse_connection_state;

typedef struct fuse_connection fuse_connection;

/* Allocates a connection in FUSE_CONN_IDLE with freshly generated
 * local/peer connection ID placeholders. Returns NULL on allocation
 * failure. */
FUSE_EXPORT fuse_connection *fuse_connection_new(void);

FUSE_EXPORT void fuse_connection_free(fuse_connection *conn);

FUSE_EXPORT fuse_connection_state fuse_connection_get_state(const fuse_connection *conn);

FUSE_EXPORT const fuse_connection_id *fuse_connection_local_cid(const fuse_connection *conn);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_CONNECTION_H */
