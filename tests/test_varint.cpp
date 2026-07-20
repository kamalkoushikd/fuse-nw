#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "fuse/varint.h"

// Test vectors from RFC 9000, Appendix A.1.
TEST(Varint, EncodeRfc9000Vectors) {
    struct Case {
        uint64_t value;
        std::vector<uint8_t> expected;
    };
    const Case cases[] = {
        {37, {0x25}},
        {15293, {0x7b, 0xbd}},
        {494878333, {0x9d, 0x7f, 0x3e, 0x7d}},
        {151288809941952652ULL, {0xc2, 0x19, 0x7c, 0x5e, 0xff, 0x14, 0xe8, 0x8c}},
    };

    for (const auto &c : cases) {
        uint8_t buf[8] = {0};
        size_t written = fuse_varint_encode(c.value, buf, sizeof(buf));
        ASSERT_EQ(written, c.expected.size()) << "value=" << c.value;
        EXPECT_TRUE(std::equal(c.expected.begin(), c.expected.end(), buf)) << "value=" << c.value;
    }
}

TEST(Varint, DecodeRfc9000Vectors) {
    struct Case {
        std::vector<uint8_t> encoded;
        uint64_t expected;
    };
    const Case cases[] = {
        {{0x25}, 37},
        {{0x7b, 0xbd}, 15293},
        {{0x9d, 0x7f, 0x3e, 0x7d}, 494878333},
        {{0xc2, 0x19, 0x7c, 0x5e, 0xff, 0x14, 0xe8, 0x8c}, 151288809941952652ULL},
    };

    for (const auto &c : cases) {
        uint64_t value = 0;
        size_t consumed = fuse_varint_decode(c.encoded.data(), c.encoded.size(), &value);
        ASSERT_EQ(consumed, c.encoded.size());
        EXPECT_EQ(value, c.expected);
    }
}

TEST(Varint, RoundTrip) {
    const uint64_t values[] = {0, 1, 63, 64, 16383, 16384, 1073741823, 1073741824,
                               4611686018427387903ULL};
    for (uint64_t v : values) {
        uint8_t buf[8];
        size_t written = fuse_varint_encode(v, buf, sizeof(buf));
        ASSERT_NE(written, 0u);

        uint64_t decoded = 0;
        size_t consumed = fuse_varint_decode(buf, written, &decoded);
        EXPECT_EQ(consumed, written);
        EXPECT_EQ(decoded, v);
    }
}

TEST(Varint, RejectsOutOfRangeValue) {
    uint8_t buf[8];
    EXPECT_EQ(fuse_varint_encode(0x4000000000000000ULL, buf, sizeof(buf)), 0u);
}

TEST(Varint, RejectsBufferTooSmall) {
    uint8_t buf[1];
    EXPECT_EQ(fuse_varint_encode(15293, buf, sizeof(buf)), 0u);
}

TEST(Varint, DecodeRejectsTruncatedInput) {
    const uint8_t truncated[] = {0xc2, 0x19, 0x7c};
    uint64_t value = 0;
    EXPECT_EQ(fuse_varint_decode(truncated, sizeof(truncated), &value), 0u);
}
