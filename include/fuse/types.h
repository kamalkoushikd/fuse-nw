#ifndef FUSE_TYPES_H
#define FUSE_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum fuse_status {
    FUSE_OK                   = 0,
    FUSE_ERR_INVALID_ARGUMENT = -1,
    FUSE_ERR_BUFFER_TOO_SMALL = -2,
    FUSE_ERR_MALFORMED_PACKET = -3,
    FUSE_ERR_CRYPTO           = -4,
    FUSE_ERR_SOCKET           = -5,
    FUSE_ERR_NOT_SUPPORTED    = -6
} fuse_status;

/* Connection IDs are opaque, variable-length (0-20 byte) identifiers
 * chosen by each endpoint, mirroring the role they play in QUIC: they
 * let a connection survive a change of source IP/port (e.g. a client
 * roaming between Wi-Fi and cellular) since packets are demultiplexed
 * by connection ID rather than by the 4-tuple. */
#define FUSE_CONNECTION_ID_MAX_LEN 20

typedef struct fuse_connection_id {
    uint8_t data[FUSE_CONNECTION_ID_MAX_LEN];
    uint8_t len;
} fuse_connection_id;

#ifdef __cplusplus
}
#endif

#endif /* FUSE_TYPES_H */
