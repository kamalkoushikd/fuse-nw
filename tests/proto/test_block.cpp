#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "fuse/proto/block.hpp"

using namespace fuse::proto;

TEST(Block, DataDatagramRoundTrip) {
    uint8_t payload[64];
    for (int i = 0; i < 64; ++i) payload[i] = static_cast<uint8_t>(i);

    BlockHeader hdr;
    hdr.stream_id = 7;
    hdr.seq_no = 0x1122334455667788ULL;
    hdr.flags = kFlagLastBlock;
    hdr.payload_len = 64;

    uint8_t out[kMaxDatagramSize];
    size_t len = encode_data_datagram(hdr, /*send_time_ns=*/123456789ULL, payload, out, sizeof(out));
    ASSERT_EQ(len, kDataPrefixSize + 64);

    BlockHeader got;
    uint64_t send_time = 0;
    const uint8_t *got_payload = nullptr;
    ASSERT_TRUE(decode_data_datagram(out, len, &got, &send_time, &got_payload));

    EXPECT_EQ(got.stream_id, hdr.stream_id);
    EXPECT_EQ(got.seq_no, hdr.seq_no);
    EXPECT_EQ(got.flags, hdr.flags);
    EXPECT_EQ(got.payload_len, hdr.payload_len);
    EXPECT_EQ(send_time, 123456789ULL);
    ASSERT_NE(got_payload, nullptr);
    EXPECT_EQ(0, std::memcmp(got_payload, payload, 64));
}

TEST(Block, ZeroLengthPayload) {
    BlockHeader hdr;
    hdr.stream_id = 1;
    hdr.seq_no = 0;
    hdr.payload_len = 0;

    uint8_t out[kMaxDatagramSize];
    size_t len = encode_data_datagram(hdr, 0, nullptr, out, sizeof(out));
    ASSERT_EQ(len, kDataPrefixSize);

    BlockHeader got;
    uint64_t send_time = 0;
    const uint8_t *payload = reinterpret_cast<const uint8_t *>(0x1);
    ASSERT_TRUE(decode_data_datagram(out, len, &got, &send_time, &payload));
    EXPECT_EQ(got.payload_len, 0);
    EXPECT_EQ(payload, nullptr);
}

TEST(Block, RejectsOverlargePayload) {
    BlockHeader hdr;
    hdr.payload_len = kMaxPayloadSize + 1;
    uint8_t out[kMaxDatagramSize + 16];
    EXPECT_EQ(encode_data_datagram(hdr, 0, nullptr, out, sizeof(out)), 0u);
}

TEST(Block, RejectsTooSmallBuffer) {
    uint8_t payload[100] = {};
    BlockHeader hdr;
    hdr.payload_len = 100;
    uint8_t out[kDataPrefixSize + 50]; // too small for 100-byte payload
    EXPECT_EQ(encode_data_datagram(hdr, 0, payload, out, sizeof(out)), 0u);
}

TEST(Block, DecodeRejectsTruncated) {
    uint8_t payload[100] = {};
    BlockHeader hdr;
    hdr.stream_id = 1;
    hdr.payload_len = 100;
    uint8_t out[kMaxDatagramSize];
    size_t len = encode_data_datagram(hdr, 0, payload, out, sizeof(out));
    ASSERT_GT(len, 0u);

    BlockHeader got;
    uint64_t send_time = 0;
    const uint8_t *got_payload = nullptr;
    // One byte short: payload_len claims 100 but only 99 present.
    EXPECT_FALSE(decode_data_datagram(out, len - 1, &got, &send_time, &got_payload));
}

TEST(Block, DecodeRejectsWrongVersion) {
    BlockHeader hdr;
    hdr.stream_id = 1;
    hdr.payload_len = 0;
    uint8_t out[kMaxDatagramSize];
    size_t len = encode_data_datagram(hdr, 0, nullptr, out, sizeof(out));
    out[0] = 0xFE; // corrupt version

    BlockHeader got;
    uint64_t send_time = 0;
    const uint8_t *payload = nullptr;
    EXPECT_FALSE(decode_data_datagram(out, len, &got, &send_time, &payload));
}

TEST(Block, PeekMsgTypeDispatches) {
    BlockHeader hdr;
    hdr.payload_len = 0;
    uint8_t out[kMaxDatagramSize];
    size_t len = encode_data_datagram(hdr, 0, nullptr, out, sizeof(out));

    MsgType type;
    ASSERT_TRUE(peek_msg_type(out, len, &type));
    EXPECT_EQ(type, MsgType::Data);
}

// ROADMAP #11: the version-mismatch reply has a fixed layout and is
// recognised (and never answered) by any version.
TEST(Block, VersionMismatchReply) {
    uint8_t buf[8];
    ASSERT_EQ(encode_version_mismatch(buf, sizeof(buf)), kVersionMismatchSize);
    uint8_t v = 0;
    EXPECT_FALSE(decode_version_mismatch(buf, 3, &v)) << "our own version is not a mismatch";
    EXPECT_FALSE(is_foreign_version(buf, 3)) << "a mismatch reply must never be answered";

    const uint8_t from_v7[3] = {7, kVersionMismatchMarker, 7};
    ASSERT_TRUE(decode_version_mismatch(from_v7, sizeof(from_v7), &v));
    EXPECT_EQ(v, 7);
    EXPECT_FALSE(is_foreign_version(from_v7, sizeof(from_v7)));

    const uint8_t v7_data[2] = {7, static_cast<uint8_t>(MsgType::StreamStart)};
    EXPECT_TRUE(is_foreign_version(v7_data, sizeof(v7_data)));
    const uint8_t ours[2] = {kProtocolVersion, 0};
    EXPECT_FALSE(is_foreign_version(ours, sizeof(ours)));
}
