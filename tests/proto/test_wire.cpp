#include <gtest/gtest.h>

#include <cstdint>

#include "fuse/proto/wire.hpp"

using namespace fuse::proto;

TEST(Wire, U16BigEndianRoundTrip) {
    uint8_t buf[2];
    ASSERT_EQ(put_u16(buf, 0x1234), 2u);
    EXPECT_EQ(buf[0], 0x12);
    EXPECT_EQ(buf[1], 0x34);

    uint16_t v = 0;
    ASSERT_EQ(get_u16(buf, &v), 2u);
    EXPECT_EQ(v, 0x1234);
}

TEST(Wire, U64BigEndianRoundTrip) {
    uint8_t buf[8];
    const uint64_t original = 0x0102030405060708ULL;
    ASSERT_EQ(put_u64(buf, original), 8u);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(buf[i], static_cast<uint8_t>(i + 1)) << "byte " << i;
    }

    uint64_t v = 0;
    ASSERT_EQ(get_u64(buf, &v), 8u);
    EXPECT_EQ(v, original);
}

TEST(Wire, U64Extremes) {
    uint8_t buf[8];
    for (uint64_t original : {uint64_t{0}, ~uint64_t{0}, uint64_t{1}}) {
        put_u64(buf, original);
        uint64_t v = 0;
        get_u64(buf, &v);
        EXPECT_EQ(v, original);
    }
}

TEST(Wire, HeaderSizesMatchSpec) {
    // Wire v2 block header is 21 bytes:
    //   stream_id(2) + seq_no(8) + flags(1) + payload_len(2) + offset(8).
    // The offset was added in v2 so a sender can vary block size mid-stream
    // (adapting to the link) without the receiver misplacing payloads.
    EXPECT_EQ(kBlockHeaderSize, 21u);
    EXPECT_EQ(kOuterHeaderSize, 2u);
    EXPECT_EQ(kMaxWindow, 64u);
    EXPECT_EQ(kProtocolVersion, 2u);

    // The MTU-safe default stays well under a 1500-byte path MTU; the
    // ceiling is only reached by probing a link that proves it can take it.
    EXPECT_EQ(kDefaultPayloadSize, 1200u);
    EXPECT_GT(kMaxPayloadSize, kDefaultPayloadSize);
}
