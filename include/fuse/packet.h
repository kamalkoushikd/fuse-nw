#ifndef FUSE_PACKET_H
#define FUSE_PACKET_H

#include <stddef.h>
#include <stdint.h>

#include "fuse/export.h"
#include "fuse/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum fuse_packet_type {
    FUSE_PACKET_INITIAL   = 0,
    FUSE_PACKET_HANDSHAKE = 1,
    FUSE_PACKET_SHORT     = 2
} fuse_packet_type;

/* The long header carries the fields needed before a connection's
 * short-header phase begins: it identifies both endpoints' connection
 * IDs and a monotonic packet number, encoded as:
 *
 *   type (1) | version (4) | dcid_len (1) | dcid | scid_len (1) | scid
 *   | packet_number (varint)
 */
typedef struct fuse_long_header {
    fuse_packet_type type;
    uint32_t version;
    fuse_connection_id dst_cid;
    fuse_connection_id src_cid;
    uint64_t packet_number;
} fuse_long_header;

/* Encodes `hdr` into `out`. Returns the number of bytes written, or 0 if
 * `out_len` is too small or `hdr` contains an invalid field (e.g. a
 * connection ID longer than FUSE_CONNECTION_ID_MAX_LEN). */
FUSE_EXPORT size_t fuse_long_header_encode(const fuse_long_header *hdr, uint8_t *out, size_t out_len);

/* Decodes a long header from the first bytes of `in` (of which `in_len`
 * are available). On success, fills *hdr and returns the number of
 * bytes consumed. Returns 0 on a truncated or malformed header. */
FUSE_EXPORT size_t fuse_long_header_decode(const uint8_t *in, size_t in_len, fuse_long_header *hdr);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_PACKET_H */
