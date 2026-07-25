#include "fuse/proto/session_crypto.hpp"

#include <cstring>
#include <new>

#if FUSE_PROTO_WITH_DTLS
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/kdf.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/types.h>
#endif

namespace fuse::proto {

bool session_crypto_available() {
    return FUSE_PROTO_WITH_DTLS != 0;
}

#if !FUSE_PROTO_WITH_DTLS

// Built without a crypto backend: every entry point fails closed so a caller
// can never end up "encrypting" with a key it did not actually derive.
bool random_bytes(uint8_t *, size_t) { return false; }
bool derive_session_key(const uint8_t *, size_t, const uint8_t *, uint8_t *) { return false; }
LaneCipher::LaneCipher() = default;
LaneCipher::~LaneCipher() = default;
bool LaneCipher::init(const uint8_t *) { return false; }
bool LaneCipher::seal(uint16_t, uint64_t, const uint8_t *, size_t, const uint8_t *, uint16_t,
                      uint8_t *) const {
    return false;
}
bool LaneCipher::open(uint16_t, uint64_t, const uint8_t *, size_t, const uint8_t *, uint16_t,
                      uint8_t *) const {
    return false;
}

#else

namespace {

// Nonce = lane_id (2) || seq_no (8) || 0 (2), big-endian.
//
// AES-GCM catastrophically fails if a (key, nonce) pair is ever reused, so
// the construction has to make reuse structurally impossible rather than
// merely unlikely. Every block has a unique (lane, seq) within a session,
// and the session key is fresh per salt, so no nonce repeats under a key.
void build_nonce(uint16_t lane, uint64_t seq, uint8_t out[kAeadNonceLen]) {
    out[0] = static_cast<uint8_t>(lane >> 8);
    out[1] = static_cast<uint8_t>(lane);
    for (int i = 0; i < 8; ++i) {
        out[2 + i] = static_cast<uint8_t>(seq >> (56 - 8 * i));
    }
    out[10] = 0;
    out[11] = 0;
}

} // namespace

bool random_bytes(uint8_t *out, size_t len) {
    WC_RNG rng;
    if (wc_InitRng(&rng) != 0) {
        return false;
    }
    const bool ok = wc_RNG_GenerateBlock(&rng, out, static_cast<word32>(len)) == 0;
    wc_FreeRng(&rng);
    return ok;
}

bool derive_session_key(const uint8_t *psk, size_t psk_len,
                        const uint8_t salt[kSessionSaltLen],
                        uint8_t out_key[kSessionKeyLen]) {
    if (psk == nullptr || psk_len == 0 || salt == nullptr || out_key == nullptr) {
        return false;
    }
    // wc_Tls13_HKDF_Extract wants a mutable ikm; copy rather than cast away
    // the caller's const.
    uint8_t ikm[256];
    if (psk_len > sizeof(ikm)) {
        return false;
    }
    std::memcpy(ikm, psk, psk_len);

    uint8_t prk[WC_SHA256_DIGEST_SIZE];
    if (wc_Tls13_HKDF_Extract(prk, salt, static_cast<word32>(kSessionSaltLen), ikm,
                              static_cast<word32>(psk_len), WC_SHA256) != 0) {
        return false;
    }

    static const char kProtocol[] = "fuse2 ";
    static const char kLabel[] = "session key";
    return wc_Tls13_HKDF_Expand_Label(out_key, static_cast<word32>(kSessionKeyLen), prk,
                                      sizeof(prk), reinterpret_cast<const byte *>(kProtocol),
                                      sizeof(kProtocol) - 1,
                                      reinterpret_cast<const byte *>(kLabel), sizeof(kLabel) - 1,
                                      nullptr, 0, WC_SHA256) == 0;
}

LaneCipher::LaneCipher() = default;

LaneCipher::~LaneCipher() {
    if (aes_ != nullptr) {
        auto *aes = static_cast<Aes *>(aes_);
        wc_AesFree(aes);
        delete aes;
        aes_ = nullptr;
    }
}

bool LaneCipher::init(const uint8_t key[kSessionKeyLen]) {
    if (aes_ == nullptr) {
        aes_ = new (std::nothrow) Aes{};
        if (aes_ == nullptr) {
            return false;
        }
        if (wc_AesInit(static_cast<Aes *>(aes_), nullptr, INVALID_DEVID) != 0) {
            delete static_cast<Aes *>(aes_);
            aes_ = nullptr;
            return false;
        }
    }
    return wc_AesGcmSetKey(static_cast<Aes *>(aes_), key,
                           static_cast<word32>(kSessionKeyLen)) == 0;
}

bool LaneCipher::seal(uint16_t lane, uint64_t seq, const uint8_t *aad, size_t aad_len,
                      const uint8_t *pt, uint16_t pt_len, uint8_t *out) const {
    if (aes_ == nullptr || out == nullptr) {
        return false;
    }
    uint8_t nonce[kAeadNonceLen];
    build_nonce(lane, seq, nonce);
    // Tag is written directly after the ciphertext, so one datagram stays one
    // contiguous buffer and remains GSO-packable.
    return wc_AesGcmEncrypt(static_cast<Aes *>(aes_), out, pt, pt_len, nonce, kAeadNonceLen,
                            out + pt_len, kAeadTagLen, aad,
                            static_cast<word32>(aad_len)) == 0;
}

bool LaneCipher::open(uint16_t lane, uint64_t seq, const uint8_t *aad, size_t aad_len,
                      const uint8_t *in, uint16_t in_len, uint8_t *out) const {
    if (aes_ == nullptr || out == nullptr || in_len < kAeadTagLen) {
        return false;
    }
    const uint16_t ct_len = static_cast<uint16_t>(in_len - kAeadTagLen);
    uint8_t nonce[kAeadNonceLen];
    build_nonce(lane, seq, nonce);
    return wc_AesGcmDecrypt(static_cast<Aes *>(aes_), out, in, ct_len, nonce, kAeadNonceLen,
                            in + ct_len, kAeadTagLen, aad,
                            static_cast<word32>(aad_len)) == 0;
}

#endif // FUSE_PROTO_WITH_DTLS

} // namespace fuse::proto
