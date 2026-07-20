#include <gtest/gtest.h>

#include <cstring>

#include "fuse/packet.h"

namespace {

fuse_connection_id make_cid(std::initializer_list<uint8_t> bytes) {
    fuse_connection_id cid{};
    cid.len = static_cast<uint8_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), cid.data);
    return cid;
}

} // namespace

TEST(LongHeader, RoundTrip) {
    fuse_long_header hdr{};
    hdr.type = FUSE_PACKET_INITIAL;
    hdr.version = 0x00000001;
    hdr.dst_cid = make_cid({0x83, 0x94, 0xc8, 0xf0, 0x3e, 0x51, 0x57, 0x08});
    hdr.src_cid = make_cid({0x01, 0x02, 0x03});
    hdr.packet_number = 494878333;

    uint8_t buf[128];
    size_t written = fuse_long_header_encode(&hdr, buf, sizeof(buf));
    ASSERT_NE(written, 0u);

    fuse_long_header decoded{};
    size_t consumed = fuse_long_header_decode(buf, written, &decoded);
    ASSERT_EQ(consumed, written);

    EXPECT_EQ(decoded.type, hdr.type);
    EXPECT_EQ(decoded.version, hdr.version);
    EXPECT_EQ(decoded.packet_number, hdr.packet_number);
    ASSERT_EQ(decoded.dst_cid.len, hdr.dst_cid.len);
    EXPECT_EQ(0, std::memcmp(decoded.dst_cid.data, hdr.dst_cid.data, hdr.dst_cid.len));
    ASSERT_EQ(decoded.src_cid.len, hdr.src_cid.len);
    EXPECT_EQ(0, std::memcmp(decoded.src_cid.data, hdr.src_cid.data, hdr.src_cid.len));
}

TEST(LongHeader, ZeroLengthConnectionIds) {
    fuse_long_header hdr{};
    hdr.type = FUSE_PACKET_SHORT;
    hdr.version = 0;
    hdr.packet_number = 0;

    uint8_t buf[32];
    size_t written = fuse_long_header_encode(&hdr, buf, sizeof(buf));
    ASSERT_NE(written, 0u);

    fuse_long_header decoded{};
    ASSERT_EQ(fuse_long_header_decode(buf, written, &decoded), written);
    EXPECT_EQ(decoded.dst_cid.len, 0);
    EXPECT_EQ(decoded.src_cid.len, 0);
}

TEST(LongHeader, EncodeRejectsBufferTooSmall) {
    fuse_long_header hdr{};
    hdr.dst_cid = make_cid({0x01, 0x02, 0x03, 0x04});
    uint8_t buf[4];
    EXPECT_EQ(fuse_long_header_encode(&hdr, buf, sizeof(buf)), 0u);
}

TEST(LongHeader, DecodeRejectsTruncatedInput) {
    fuse_long_header hdr{};
    hdr.dst_cid = make_cid({0x01, 0x02, 0x03, 0x04});
    hdr.packet_number = 1000;

    uint8_t buf[128];
    size_t written = fuse_long_header_encode(&hdr, buf, sizeof(buf));
    ASSERT_NE(written, 0u);

    fuse_long_header decoded{};
    EXPECT_EQ(fuse_long_header_decode(buf, written - 1, &decoded), 0u);
}

TEST(LongHeader, DecodeRejectsInvalidType) {
    const uint8_t bad_type[] = {0xFF, 0, 0, 0, 0, 0, 0};
    fuse_long_header decoded{};
    EXPECT_EQ(fuse_long_header_decode(bad_type, sizeof(bad_type), &decoded), 0u);
}
