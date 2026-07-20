#include <gtest/gtest.h>

#include "fuse/crypto.h"

#if FUSE_WITH_CRYPTO

#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> hex_to_bytes(const std::string &hex) {
    std::vector<uint8_t> bytes(hex.size() / 2);
    for (size_t i = 0; i < bytes.size(); i++) {
        bytes[i] = static_cast<uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    }
    return bytes;
}

} // namespace

// These vectors are RFC 9001 Appendix A.1's published QUIC v1 Initial-key
// test values. They exercise fuse's HKDF-Extract / HKDF-Expand-Label
// primitives (which fuse's own protocol-specific derivation is built on)
// against an independently verifiable, standardized answer rather than a
// value only this codebase could reproduce.
TEST(Crypto, MatchesRfc9001InitialSecretVectors) {
    const auto salt = hex_to_bytes("38762cf7f55934b34d179ae6a4c80cadccbb7f0a");
    const auto cid = hex_to_bytes("8394c8f03e515708");

    uint8_t initial_secret[FUSE_SHA256_LEN];
    ASSERT_EQ(fuse_crypto_hkdf_extract(FUSE_HASH_SHA256, salt.data(), salt.size(), cid.data(),
                                       cid.size(), initial_secret, sizeof(initial_secret)),
              FUSE_OK);

    const auto expected_initial_secret =
        hex_to_bytes("7db5df06e7a69e432496adedb008519"
                     "23595221596ae2ae9fb8115c1e9ed0a44");
    // The RFC prints this value split across two indented lines; the
    // concatenation above must be exactly 32 bytes (64 hex chars).
    ASSERT_EQ(expected_initial_secret.size(), FUSE_SHA256_LEN);
    EXPECT_EQ(0, std::memcmp(initial_secret, expected_initial_secret.data(), FUSE_SHA256_LEN));

    uint8_t client_secret[FUSE_SHA256_LEN];
    ASSERT_EQ(fuse_crypto_hkdf_expand_label(FUSE_HASH_SHA256, initial_secret, sizeof(initial_secret),
                                            "tls13 ", 6, "client in", 9, nullptr, 0, client_secret,
                                            sizeof(client_secret)),
              FUSE_OK);
    const auto expected_client_secret =
        hex_to_bytes("c00cf151ca5be075ed0ebfb5c80323c4"
                     "2d6b7db67881289af4008f1f6c357aea");
    EXPECT_EQ(0, std::memcmp(client_secret, expected_client_secret.data(), FUSE_SHA256_LEN));

    uint8_t server_secret[FUSE_SHA256_LEN];
    ASSERT_EQ(fuse_crypto_hkdf_expand_label(FUSE_HASH_SHA256, initial_secret, sizeof(initial_secret),
                                            "tls13 ", 6, "server in", 9, nullptr, 0, server_secret,
                                            sizeof(server_secret)),
              FUSE_OK);
    const auto expected_server_secret =
        hex_to_bytes("3c199828fd139efd216c155ad844cc81"
                     "fb82fa8d7446fa7d78be803acdda951b");
    EXPECT_EQ(0, std::memcmp(server_secret, expected_server_secret.data(), FUSE_SHA256_LEN));
}

TEST(Crypto, DeriveInitialSecretsIsDeterministicAndDistinct) {
    fuse_connection_id cid{};
    cid.len = 8;
    for (int i = 0; i < 8; i++) cid.data[i] = static_cast<uint8_t>(i);

    uint8_t client_a[FUSE_SHA256_LEN], server_a[FUSE_SHA256_LEN];
    uint8_t client_b[FUSE_SHA256_LEN], server_b[FUSE_SHA256_LEN];

    ASSERT_EQ(fuse_crypto_derive_initial_secrets(&cid, client_a, sizeof(client_a), server_a,
                                                 sizeof(server_a)),
              FUSE_OK);
    ASSERT_EQ(fuse_crypto_derive_initial_secrets(&cid, client_b, sizeof(client_b), server_b,
                                                 sizeof(server_b)),
              FUSE_OK);

    EXPECT_EQ(0, std::memcmp(client_a, client_b, FUSE_SHA256_LEN));
    EXPECT_EQ(0, std::memcmp(server_a, server_b, FUSE_SHA256_LEN));
    EXPECT_NE(0, std::memcmp(client_a, server_a, FUSE_SHA256_LEN));
}

TEST(Crypto, RejectsWrongOutputLength) {
    fuse_connection_id cid{};
    cid.len = 4;
    uint8_t too_short[FUSE_SHA256_LEN - 1];
    uint8_t ok_len[FUSE_SHA256_LEN];
    EXPECT_EQ(fuse_crypto_derive_initial_secrets(&cid, too_short, sizeof(too_short), ok_len,
                                                 sizeof(ok_len)),
              FUSE_ERR_INVALID_ARGUMENT);
}

#else // !FUSE_WITH_CRYPTO

TEST(Crypto, ReportsUnavailableWhenBuiltWithoutCrypto) {
    EXPECT_EQ(fuse_crypto_available(), 0);

    fuse_connection_id cid{};
    uint8_t client_secret[FUSE_SHA256_LEN];
    uint8_t server_secret[FUSE_SHA256_LEN];
    EXPECT_EQ(fuse_crypto_derive_initial_secrets(&cid, client_secret, sizeof(client_secret),
                                                 server_secret, sizeof(server_secret)),
              FUSE_ERR_NOT_SUPPORTED);
}

#endif // FUSE_WITH_CRYPTO
