#ifndef FUSE_PROTO_SESSION_CRYPTO_HPP
#define FUSE_PROTO_SESSION_CRYPTO_HPP

// Session-wide keying with per-lane parallel AEAD.
//
// The obvious way to encrypt a sharded transfer is to give each lane its own
// DTLS session. That is wrong twice over:
//
//   * A DTLS session owns record-layer state (sequence numbers, epoch,
//     replay window, cipher state). It cannot be shared across lanes without
//     a mutex, and a mutex serialises exactly the parallelism the sharding
//     exists to create.
//   * Routing data through wolfSSL_write hands socket ownership to DTLS, one
//     record per call — which destroys the UDP GSO batching that the
//     throughput depends on.
//
// So: derive keys ONCE for the whole session (one handshake's worth of
// negotiation, not N), then let every lane encrypt independently with the
// same key and a nonce that is unique per (lane, sequence). Ciphertext
// blocks are still fixed-size datagrams, so they pack into a GSO batch
// exactly as plaintext did. This is the same shape fast QUIC stacks use:
// per-packet AEAD, then batch the already-encrypted packets into one send.
//
// SECURITY NOTE. Keys here come from a pre-shared key plus a per-session
// random salt. The salt gives key freshness — two sessions never reuse a
// key — but this is NOT forward secrecy: an attacker who records traffic and
// later obtains the PSK can decrypt it. The DTLS path (dtls.hpp) negotiates
// ECDHE-PSK and *does* provide forward secrecy. This module trades that
// property for throughput; choose it only where that trade is acceptable.

#include <cstddef>
#include <cstdint>

namespace fuse::proto {

inline constexpr size_t kSessionKeyLen = 32; // AES-256
inline constexpr size_t kSessionSaltLen = 16;
inline constexpr size_t kAeadNonceLen = 12;
inline constexpr size_t kAeadTagLen = 16;

// True when this build has the AEAD backend compiled in.
bool session_crypto_available();

// Fills `out` with cryptographically random bytes. Returns false if no
// secure source is available — callers must fail closed rather than fall
// back to a predictable salt.
bool random_bytes(uint8_t *out, size_t len);

// Derives the session key from a pre-shared key and a per-session salt
// (HKDF). Both endpoints run this over the same inputs and get the same key.
bool derive_session_key(const uint8_t *psk, size_t psk_len,
                        const uint8_t salt[kSessionSaltLen],
                        uint8_t out_key[kSessionKeyLen]);

// One lane's cipher. Thread-confined: each lane owns one and never shares
// it, which is what keeps encryption parallel. All lanes hold the same
// session key; nonce separation comes from (lane_id, seq_no).
class LaneCipher {
public:
    LaneCipher();
    ~LaneCipher();
    LaneCipher(const LaneCipher &) = delete;
    LaneCipher &operator=(const LaneCipher &) = delete;

    bool init(const uint8_t key[kSessionKeyLen]);

    // Encrypts `pt_len` bytes into `out`, which must have room for
    // pt_len + kAeadTagLen. `aad` is authenticated but not encrypted — pass
    // the block's identifying fields so a block cannot be replayed at a
    // different position.
    bool seal(uint16_t lane, uint64_t seq, const uint8_t *aad, size_t aad_len,
              const uint8_t *pt, uint16_t pt_len, uint8_t *out) const;

    // Reverses seal(). `in_len` includes the tag; `out` needs
    // in_len - kAeadTagLen bytes. Returns false if authentication fails,
    // which is the only correct response to a tampered or corrupt block.
    bool open(uint16_t lane, uint64_t seq, const uint8_t *aad, size_t aad_len,
              const uint8_t *in, uint16_t in_len, uint8_t *out) const;

private:
    void *aes_ = nullptr; // wolfSSL Aes, kept opaque to avoid leaking the dep
};

} // namespace fuse::proto

#endif // FUSE_PROTO_SESSION_CRYPTO_HPP
