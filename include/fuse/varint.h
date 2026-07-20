#ifndef FUSE_VARINT_H
#define FUSE_VARINT_H

#include <stddef.h>
#include <stdint.h>

#include "fuse/export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* QUIC-style variable-length integer encoding (RFC 9000, Section 16):
 * the two most significant bits of the first byte select a 1/2/4/8-byte
 * encoding, giving a compact wire format for the small values (stream
 * IDs, ACK ranges, lengths, ...) that dominate transport-protocol
 * traffic while still allowing values up to 2^62-1. */

/* Returns the encoded length (1, 2, 4, or 8) for `value`, or 0 if it
 * exceeds the maximum representable value (2^62 - 1). */
FUSE_EXPORT size_t fuse_varint_encoded_len(uint64_t value);

/* Encodes `value` into `out`. `out_len` must be at least
 * fuse_varint_encoded_len(value). Returns the number of bytes written,
 * or 0 on error (buffer too small, or value out of range). */
FUSE_EXPORT size_t fuse_varint_encode(uint64_t value, uint8_t *out, size_t out_len);

/* Decodes a varint from the first bytes of `in` (of which `in_len` are
 * available). On success, stores the value in *value and returns the
 * number of bytes consumed. Returns 0 if `in_len` is too small to
 * contain a complete varint. */
FUSE_EXPORT size_t fuse_varint_decode(const uint8_t *in, size_t in_len, uint64_t *value);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_VARINT_H */
