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

// SipHash-2-4: a keyed pseudorandom function, for values that must not be
// computable or invertible without the key (address-validation tokens).
// FNV-1a above is invertible, so a "secret-prefixed FNV" token would let an
// attacker recover the hash state from one token issued to their own
// address and then mint tokens for anyone else's.
inline uint64_t siphash24(const uint8_t key[16], const uint8_t *data, size_t len) {
    const auto rd = [](const uint8_t *p) {
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
        return v;
    };
    const auto rotl = [](uint64_t x, int b) { return (x << b) | (x >> (64 - b)); };
    const uint64_t k0 = rd(key), k1 = rd(key + 8);
    uint64_t v0 = 0x736f6d6570736575ull ^ k0, v1 = 0x646f72616e646f6dull ^ k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ k0, v3 = 0x7465646279746573ull ^ k1;
    const auto round = [&] {
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
    };
    const size_t whole = len - len % 8;
    for (size_t i = 0; i < whole; i += 8) {
        const uint64_t m = rd(data + i);
        v3 ^= m;
        round();
        round();
        v0 ^= m;
    }
    uint64_t last = static_cast<uint64_t>(len) << 56;
    for (size_t i = 0; i < len % 8; ++i) last |= static_cast<uint64_t>(data[whole + i]) << (8 * i);
    v3 ^= last;
    round();
    round();
    v0 ^= last;
    v2 ^= 0xff;
    round();
    round();
    round();
    round();
    return v0 ^ v1 ^ v2 ^ v3;
}

} // namespace fuse::proto

#endif // FUSE_PROTO_HASH_HPP
