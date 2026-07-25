#include <gtest/gtest.h>

#include <cstring>
#define _GNU_SOURCE
#include <string.h>
#include <set>
#include <string>
#include <vector>

#include "fuse/proto/session_crypto.hpp"

using namespace fuse::proto;

namespace {
const std::string kPsk = "fuse-session-preshared-key";

struct Keyed {
    uint8_t salt[kSessionSaltLen]{};
    uint8_t key[kSessionKeyLen]{};
};

Keyed make_keys() {
    Keyed k;
    EXPECT_TRUE(random_bytes(k.salt, sizeof(k.salt)));
    EXPECT_TRUE(derive_session_key(reinterpret_cast<const uint8_t *>(kPsk.data()), kPsk.size(),
                                   k.salt, k.key));
    return k;
}
} // namespace

#if FUSE_PROTO_WITH_DTLS

TEST(SessionCrypto, Available) { EXPECT_TRUE(session_crypto_available()); }

TEST(SessionCrypto, SameInputsDeriveSameKeyOnBothEnds) {
    uint8_t salt[kSessionSaltLen];
    ASSERT_TRUE(random_bytes(salt, sizeof(salt)));

    uint8_t a[kSessionKeyLen], b[kSessionKeyLen];
    ASSERT_TRUE(derive_session_key(reinterpret_cast<const uint8_t *>(kPsk.data()), kPsk.size(),
                                   salt, a));
    ASSERT_TRUE(derive_session_key(reinterpret_cast<const uint8_t *>(kPsk.data()), kPsk.size(),
                                   salt, b));
    EXPECT_EQ(0, std::memcmp(a, b, kSessionKeyLen))
        << "both endpoints must derive an identical key from the same PSK and salt";
}

// Key freshness: a different salt must yield a different key, so two
// sessions never encrypt under the same key.
TEST(SessionCrypto, DifferentSaltYieldsDifferentKey) {
    Keyed x = make_keys();
    Keyed y = make_keys();
    EXPECT_NE(0, std::memcmp(x.salt, y.salt, kSessionSaltLen)) << "salts should differ";
    EXPECT_NE(0, std::memcmp(x.key, y.key, kSessionKeyLen))
        << "a fresh salt must produce a fresh session key";
}

TEST(SessionCrypto, SealOpenRoundTrip) {
    Keyed k = make_keys();
    LaneCipher c;
    ASSERT_TRUE(c.init(k.key));

    const std::string msg = "the quick brown fox jumps over the lazy dog, repeatedly";
    const uint8_t aad[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    std::vector<uint8_t> ct(msg.size() + kAeadTagLen);
    ASSERT_TRUE(c.seal(3, 42, aad, sizeof(aad),
                       reinterpret_cast<const uint8_t *>(msg.data()),
                       static_cast<uint16_t>(msg.size()), ct.data()));

    // The ciphertext must not contain the plaintext.
    EXPECT_EQ(nullptr, memmem(ct.data(), ct.size(), msg.data(), msg.size()))
        << "plaintext must not survive in the ciphertext";

    std::vector<uint8_t> pt(msg.size());
    ASSERT_TRUE(c.open(3, 42, aad, sizeof(aad), ct.data(), static_cast<uint16_t>(ct.size()),
                       pt.data()));
    EXPECT_EQ(0, std::memcmp(pt.data(), msg.data(), msg.size()));
}

// All lanes share one session key; separation comes from the nonce. Two
// lanes encrypting identical plaintext at the same seq must still produce
// different ciphertext, or the nonce construction is broken.
TEST(SessionCrypto, LanesShareKeyButNotNonceSpace) {
    Keyed k = make_keys();
    LaneCipher lane0, lane1;
    ASSERT_TRUE(lane0.init(k.key));
    ASSERT_TRUE(lane1.init(k.key));

    const uint8_t pt[32] = {};
    std::vector<uint8_t> c0(sizeof(pt) + kAeadTagLen), c1(sizeof(pt) + kAeadTagLen);
    ASSERT_TRUE(lane0.seal(0, 7, nullptr, 0, pt, sizeof(pt), c0.data()));
    ASSERT_TRUE(lane1.seal(1, 7, nullptr, 0, pt, sizeof(pt), c1.data()));

    EXPECT_NE(0, std::memcmp(c0.data(), c1.data(), c0.size()))
        << "same key + same seq on different lanes must not repeat a nonce";
}

TEST(SessionCrypto, SequenceSeparatesNonces) {
    Keyed k = make_keys();
    LaneCipher c;
    ASSERT_TRUE(c.init(k.key));
    const uint8_t pt[16] = {};

    std::set<std::string> seen;
    for (uint64_t seq = 0; seq < 64; ++seq) {
        std::vector<uint8_t> ct(sizeof(pt) + kAeadTagLen);
        ASSERT_TRUE(c.seal(0, seq, nullptr, 0, pt, sizeof(pt), ct.data()));
        seen.insert(std::string(reinterpret_cast<char *>(ct.data()), ct.size()));
    }
    EXPECT_EQ(seen.size(), 64u) << "every sequence number must produce distinct ciphertext";
}

// Authentication must actually reject tampering — otherwise the AEAD is
// providing confidentiality only, and silently.
TEST(SessionCrypto, RejectsTamperedCiphertext) {
    Keyed k = make_keys();
    LaneCipher c;
    ASSERT_TRUE(c.init(k.key));

    const uint8_t pt[64] = {};
    std::vector<uint8_t> ct(sizeof(pt) + kAeadTagLen);
    ASSERT_TRUE(c.seal(0, 1, nullptr, 0, pt, sizeof(pt), ct.data()));

    ct[5] ^= 0x01; // flip one bit of ciphertext
    std::vector<uint8_t> out(sizeof(pt));
    EXPECT_FALSE(c.open(0, 1, nullptr, 0, ct.data(), static_cast<uint16_t>(ct.size()), out.data()))
        << "a modified block must fail authentication";
}

TEST(SessionCrypto, RejectsTamperedTag) {
    Keyed k = make_keys();
    LaneCipher c;
    ASSERT_TRUE(c.init(k.key));
    const uint8_t pt[64] = {};
    std::vector<uint8_t> ct(sizeof(pt) + kAeadTagLen);
    ASSERT_TRUE(c.seal(0, 1, nullptr, 0, pt, sizeof(pt), ct.data()));

    ct.back() ^= 0x80;
    std::vector<uint8_t> out(sizeof(pt));
    EXPECT_FALSE(c.open(0, 1, nullptr, 0, ct.data(), static_cast<uint16_t>(ct.size()), out.data()));
}

// The AAD binds a block to its position. Replaying a valid block at a
// different offset/seq must fail, or an attacker could reorder the file.
TEST(SessionCrypto, AadBindsBlockToItsPosition) {
    Keyed k = make_keys();
    LaneCipher c;
    ASSERT_TRUE(c.init(k.key));

    const uint8_t pt[48] = {};
    const uint8_t aad_a[4] = {0, 0, 0, 1};
    const uint8_t aad_b[4] = {0, 0, 0, 2};

    std::vector<uint8_t> ct(sizeof(pt) + kAeadTagLen);
    ASSERT_TRUE(c.seal(0, 9, aad_a, sizeof(aad_a), pt, sizeof(pt), ct.data()));

    std::vector<uint8_t> out(sizeof(pt));
    EXPECT_FALSE(c.open(0, 9, aad_b, sizeof(aad_b), ct.data(),
                        static_cast<uint16_t>(ct.size()), out.data()))
        << "a block must not authenticate under different associated data";
    EXPECT_TRUE(c.open(0, 9, aad_a, sizeof(aad_a), ct.data(),
                       static_cast<uint16_t>(ct.size()), out.data()));
}

TEST(SessionCrypto, WrongSeqFailsAuthentication) {
    Keyed k = make_keys();
    LaneCipher c;
    ASSERT_TRUE(c.init(k.key));
    const uint8_t pt[32] = {};
    std::vector<uint8_t> ct(sizeof(pt) + kAeadTagLen);
    ASSERT_TRUE(c.seal(0, 5, nullptr, 0, pt, sizeof(pt), ct.data()));

    std::vector<uint8_t> out(sizeof(pt));
    EXPECT_FALSE(c.open(0, 6, nullptr, 0, ct.data(), static_cast<uint16_t>(ct.size()), out.data()))
        << "decrypting under the wrong sequence number must fail";
}

TEST(SessionCrypto, WrongKeyFails) {
    Keyed a = make_keys(), b = make_keys();
    LaneCipher ca, cb;
    ASSERT_TRUE(ca.init(a.key));
    ASSERT_TRUE(cb.init(b.key));

    const uint8_t pt[32] = {};
    std::vector<uint8_t> ct(sizeof(pt) + kAeadTagLen);
    ASSERT_TRUE(ca.seal(0, 1, nullptr, 0, pt, sizeof(pt), ct.data()));

    std::vector<uint8_t> out(sizeof(pt));
    EXPECT_FALSE(cb.open(0, 1, nullptr, 0, ct.data(), static_cast<uint16_t>(ct.size()),
                         out.data()));
}

#else

TEST(SessionCrypto, FailsClosedWithoutBackend) {
    EXPECT_FALSE(session_crypto_available());
    uint8_t salt[kSessionSaltLen] = {};
    uint8_t key[kSessionKeyLen] = {};
    EXPECT_FALSE(random_bytes(salt, sizeof(salt)));
    EXPECT_FALSE(derive_session_key(reinterpret_cast<const uint8_t *>("x"), 1, salt, key));
    LaneCipher c;
    EXPECT_FALSE(c.init(key));
}

#endif
