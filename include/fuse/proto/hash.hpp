#ifndef FUSE_PROTO_HASH_HPP
#define FUSE_PROTO_HASH_HPP

// A small, dependency-free 64-bit hash (FNV-1a) used by the Stage 2 SETUP
// handshake to detect corruption of the negotiated configuration in
// transit. This is an integrity check, NOT authentication: it catches a
// garbled SETUP payload, not a malicious one. Confidentiality/authenticity
// is the job of the optional DTLS layer (Stage 7), not this hash.

#include <cstddef>
#include <cstdint>

namespace fuse::proto {

inline uint64_t hash64(const uint8_t *data, size_t len) {
    uint64_t h = 1469598103934665603ull; // FNV offset basis
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ull; // FNV prime
    }
    return h;
}

} // namespace fuse::proto

#endif // FUSE_PROTO_HASH_HPP
