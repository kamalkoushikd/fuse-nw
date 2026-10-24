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
                                   k.salt, sizeof(k.salt), k.key));
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
                                   salt, sizeof(salt), a));
    ASSERT_TRUE(derive_session_key(reinterpret_cast<const uint8_t *>(kPsk.data()), kPsk.size(),
                                   salt, sizeof(salt), b));
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
    EXPECT_FALSE(derive_session_key(reinterpret_cast<const uint8_t *>("x"), 1, salt, sizeof(salt),
                                    key));
    LaneCipher c;
    EXPECT_FALSE(c.init(key));
}

#endif

// Protocol nonces must come from a secure source in every build, not a
// seeded PRNG (ROADMAP #20). Collisions among a few thousand draws from a
// real 64-bit source are vanishingly unlikely.
TEST(SessionCrypto, SecureRandomIsAvailableAndVaries) {
    std::set<uint64_t> seen;
    for (int i = 0; i < 4096; ++i) seen.insert(secure_random_u64());
    EXPECT_EQ(seen.size(), 4096u);
}

// A pre-shared key longer than 256 bytes used to fail key derivation (and
// surface as "Unsupported") because it was copied into a fixed buffer.
TEST(SessionCrypto, LongPreSharedKeyDerives) {
    if (!session_crypto_available()) GTEST_SKIP() << "built without crypto";
    std::vector<uint8_t> psk(4096);
    for (size_t i = 0; i < psk.size(); ++i) psk[i] = static_cast<uint8_t>(i * 13);
    uint8_t salt[kSessionSaltLen] = {1, 2, 3};
    uint8_t a[kSessionKeyLen], b[kSessionKeyLen];
    ASSERT_TRUE(derive_session_key(psk.data(), psk.size(), salt, sizeof(salt), a));
    ASSERT_TRUE(derive_session_key(psk.data(), psk.size(), salt, sizeof(salt), b));
    EXPECT_EQ(std::memcmp(a, b, sizeof(a)), 0);
    psk.back() ^= 1;
    ASSERT_TRUE(derive_session_key(psk.data(), psk.size(), salt, sizeof(salt), b));
    EXPECT_NE(std::memcmp(a, b, sizeof(a)), 0);
}

// Key epochs (ROADMAP #21): a lane switches to a freshly derived key every
// 2^kKeyEpochShift blocks, identically on both ends, with nothing on the wire.
TEST(SessionCrypto, KeyEpochsRotateDeterministically) {
    if (!session_crypto_available()) GTEST_SKIP() << "built without crypto";
    uint8_t key[kSessionKeyLen];
    for (size_t i = 0; i < sizeof(key); ++i) key[i] = static_cast<uint8_t>(i);
    LaneKeySchedule tx, rx;
    ASSERT_TRUE(tx.init(key, 3));
    ASSERT_TRUE(rx.init(key, 3));
    EXPECT_FALSE(tx.init(key, 3)) << "a schedule must not be re-keyed";

    const uint8_t pt[16] = {'e', 'p', 'o', 'c', 'h'};
    const uint64_t last_of_0 = (1ull << kKeyEpochShift) - 1, first_of_1 = 1ull << kKeyEpochShift;
    for (uint64_t seq : {uint64_t{0}, last_of_0, first_of_1, uint64_t{5} << kKeyEpochShift}) {
        uint8_t ct[sizeof(pt) + kAeadTagLen], back[sizeof(pt)];
        const LaneCipher *c = tx.cipher_for(seq);
        ASSERT_NE(c, nullptr);
        ASSERT_TRUE(c->seal(3, seq, nullptr, 0, pt, sizeof(pt), ct));
        const LaneCipher *d = rx.cipher_for(seq);
        ASSERT_NE(d, nullptr);
        ASSERT_TRUE(d->open(3, seq, nullptr, 0, ct, sizeof(ct), back)) << "seq " << seq;
        EXPECT_EQ(std::memcmp(back, pt, sizeof(pt)), 0);
    }

    // Different epochs really use different keys: epoch 1's cipher cannot
    // open a block sealed under epoch 0's key, even at the same nonce.
    uint8_t ct[sizeof(pt) + kAeadTagLen], back[sizeof(pt)];
    ASSERT_TRUE(tx.cipher_for(0)->seal(3, 0, nullptr, 0, pt, sizeof(pt), ct));
    EXPECT_FALSE(rx.cipher_for(first_of_1)->open(3, 0, nullptr, 0, ct, sizeof(ct), back));

    uint8_t k0[kSessionKeyLen], k1[kSessionKeyLen], other_lane[kSessionKeyLen];
    ASSERT_TRUE(derive_lane_epoch_key(key, 3, 0, k0));
    ASSERT_TRUE(derive_lane_epoch_key(key, 3, 1, k1));
    ASSERT_TRUE(derive_lane_epoch_key(key, 4, 0, other_lane));
    EXPECT_NE(std::memcmp(k0, k1, sizeof(k0)), 0);
    EXPECT_NE(std::memcmp(k0, other_lane, sizeof(k0)), 0);
}
