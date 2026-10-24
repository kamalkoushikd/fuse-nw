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
#include <memory>

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

// An unpredictable 64-bit value for protocol nonces and tokens (StreamStart
// nonces, path challenges, address-validation secrets). Available in every
// build: it uses the crypto backend's RNG when there is one and the
// kernel's getrandom() otherwise. Never a seeded PRNG, whose future outputs
// can be predicted from a few observed ones.
uint64_t secure_random_u64();

// Derives the session key from a pre-shared key and salt material (HKDF).
// Both endpoints must pass byte-identical salt and get the same key. `salt`
// need not be a single kSessionSaltLen buffer: a caller that wants the key
// to depend on randomness from both peers (so neither one alone determines
// it — see sdk.cpp's fuse_accept/fuse_connect) passes the concatenation of
// both sides' salts.
bool derive_session_key(const uint8_t *psk, size_t psk_len, const uint8_t *salt, size_t salt_len,
                        uint8_t out_key[kSessionKeyLen]);

// One lane's cipher. Thread-confined: each lane owns one and never shares
// it, which is what keeps encryption parallel. All lanes hold the same
// session key; nonce separation comes from (lane_id, seq_no).
class ControlAuth;

class LaneCipher {
public:
    LaneCipher();
    ~LaneCipher();
    LaneCipher(const LaneCipher &) = delete;
    LaneCipher &operator=(const LaneCipher &) = delete;

    // Fails if this instance was already initialised: nonce uniqueness rests
    // on (key, seq_no) never repeating, and re-initialising the same object
    // (e.g. a session reset that reuses a salt) is exactly how a caller could
    // silently break that guarantee. Construct a fresh LaneCipher instead.
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
    friend class ControlAuth; // computes GMAC tags with the same AES context

    void *aes_ = nullptr; // wolfSSL Aes, kept opaque to avoid leaking the dep
    bool  initialized_ = false;
};

// Control-plane authentication (ROADMAP #2). Under a pre-shared key, every
// control datagram (StreamStart, Ack, Nack, ResumeRanges, StreamClose,
// FileDigest, path challenges) carries a trailer
//
//     [... the datagram as encoded ...][ctr:8][tag:16]
//
// where tag is AES-GCM with an empty plaintext (i.e. GMAC) over everything
// before it, under a *control* key derived separately from the data key,
// with nonce  lane:2 || ctr:8 || dir:1 || 0x01.  Data nonces end in 0x0000,
// so the two nonce spaces are disjoint even before the keys differ. `dir`
// separates the two ends (0 = data sender, 1 = data receiver), which share
// the key and the lane. The counter only has to be unique per sender;
// replaying an old authentic control message is harmless (ACK bases only
// move forward, and an abort can only be replayed if one was really sent).
inline constexpr size_t kControlTrailerLen = 8 + kAeadTagLen;

class ControlAuth {
public:
    ControlAuth() = default;
    ControlAuth(const ControlAuth &) = delete;
    ControlAuth &operator=(const ControlAuth &) = delete;

    // `local_dir` is this end's direction (0 sender, 1 receiver); messages
    // are verified as coming from the other one.
    bool init(const uint8_t session_key[kSessionKeyLen], uint16_t lane, uint8_t local_dir);
    bool ready() const { return cipher_ != nullptr; }

    // Appends the trailer to the `len`-byte datagram in `dg` (which must have
    // room for len + kControlTrailerLen). Returns the new length, 0 on failure.
    size_t seal(uint8_t *dg, size_t len, size_t cap);

    // Verifies a received datagram's trailer. On success returns true and
    // sets *body_len to the datagram length without the trailer.
    bool open(const uint8_t *dg, size_t len, size_t *body_len) const;

private:
    std::unique_ptr<LaneCipher> cipher_;
    uint16_t lane_ = 0;
    uint8_t dir_ = 0;
    uint64_t next_ctr_ = 1;
};

// Key usage limit (ROADMAP #21). One AES-GCM key must not protect an
// unbounded number of records: the commonly applied confidentiality limit
// is about 2^23 packets (RFC 9001 §6.6). Rather than rekey with extra
// messages, each lane's key is a deterministic function of the session key,
// the lane and the *epoch* (seq >> kKeyEpochShift), so both ends switch
// keys at the same sequence number with nothing on the wire. Nonces stay
// (lane, seq), unique across epochs anyway.
inline constexpr unsigned kKeyEpochShift = 22; // 4,194,304 blocks per key

bool derive_lane_epoch_key(const uint8_t session_key[kSessionKeyLen], uint16_t lane,
                           uint64_t epoch, uint8_t out_key[kSessionKeyLen]);

// The ciphers of one lane across key epochs. Thread-confined like
// LaneCipher. It keeps the two most recently used epochs, so a
// retransmission of a block from just before an epoch boundary does not
// force a re-derive on every resend.
class LaneKeySchedule {
public:
    LaneKeySchedule() = default;
    LaneKeySchedule(const LaneKeySchedule &) = delete;
    LaneKeySchedule &operator=(const LaneKeySchedule &) = delete;

    bool init(const uint8_t session_key[kSessionKeyLen], uint16_t lane);

    // The cipher for the epoch `seq` belongs to; nullptr if it could not be
    // derived (which callers must treat as an authentication failure).
    const LaneCipher *cipher_for(uint64_t seq);

private:
    struct Slot {
        uint64_t epoch = UINT64_MAX;
        std::unique_ptr<LaneCipher> cipher;
    };
    uint8_t key_[kSessionKeyLen] = {};
    uint16_t lane_ = 0;
    bool ready_ = false;
    Slot slots_[2];
    unsigned next_ = 0;
};

} // namespace fuse::proto

#endif // FUSE_PROTO_SESSION_CRYPTO_HPP
