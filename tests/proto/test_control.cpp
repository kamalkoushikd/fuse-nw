#include <gtest/gtest.h>

#include <cstring>

#include "fuse/proto/control.hpp"

using namespace fuse::proto;

TEST(Aux, HeartbeatRoundTrip) {
    Heartbeat hb;
    hb.stream_id = 42;
    hb.highest_seq_no = 0xDEADBEEFCAFEULL;

    uint8_t buf[kMaxAuxDatagramSize];
    size_t len = encode_heartbeat(hb, buf, sizeof(buf));
    ASSERT_GT(len, 0u);

    Heartbeat got;
    ASSERT_TRUE(decode_heartbeat(buf, len, &got));
    EXPECT_EQ(got.stream_id, hb.stream_id);
    EXPECT_EQ(got.highest_seq_no, hb.highest_seq_no);
}

TEST(Aux, StreamStartRoundTrip) {
    StreamStart ss;
    ss.stream_id = 7;
    ss.total_blocks = 6991;
    ss.block_size = 1200;
    ss.total_bytes = 8388608;
    ss.nonce = 0xA5A5A5A5A5A5A5A5ULL;
    ss.stream_base_offset = 3 * 8388608ULL;
    ss.file_total_bytes = 4 * 8388608ULL + 17;

    uint8_t buf[kMaxAuxDatagramSize];
    size_t len = encode_stream_start(ss, buf, sizeof(buf));
    ASSERT_GT(len, 0u);

    StreamStart got;
    ASSERT_TRUE(decode_stream_start(buf, len, &got));
    EXPECT_EQ(got.stream_id, ss.stream_id);
    EXPECT_EQ(got.total_blocks, ss.total_blocks);
    EXPECT_EQ(got.block_size, ss.block_size);
    EXPECT_EQ(got.total_bytes, ss.total_bytes);
    EXPECT_EQ(got.nonce, ss.nonce);
    EXPECT_EQ(got.stream_base_offset, ss.stream_base_offset);
    EXPECT_EQ(got.file_total_bytes, ss.file_total_bytes);

    // A pre-v6 (shorter) StreamStart must not decode.
    EXPECT_FALSE(decode_stream_start(buf, len - 16, &got));
}

TEST(Aux, StreamStartDoesNotDecodeAsOtherTypes) {
    StreamStart ss;
    ss.stream_id = 1;
    uint8_t buf[kMaxAuxDatagramSize];
    size_t len = encode_stream_start(ss, buf, sizeof(buf));
    Ack ack;
    EXPECT_FALSE(decode_ack(buf, len, &ack));
}

TEST(Aux, AckRoundTrip) {
    Ack ack;
    ack.stream_id = 3;
    ack.base_seq_no = 1000;
    ack.received_bitmask[0] = 0b10110;
    ack.echoed_send_time = 55555;
    ack.nonce = 0xdeadbeefULL;

    uint8_t buf[kMaxAuxDatagramSize];
    size_t len = encode_ack(ack, buf, sizeof(buf));
    ASSERT_GT(len, 0u);

    Ack got;
    ASSERT_TRUE(decode_ack(buf, len, &got));
    EXPECT_EQ(got.stream_id, ack.stream_id);
    EXPECT_EQ(got.base_seq_no, ack.base_seq_no);
    for (size_t i = 0; i < kMaskWords; ++i) {
        EXPECT_EQ(got.received_bitmask[i], ack.received_bitmask[i]);
    }
    EXPECT_EQ(got.echoed_send_time, ack.echoed_send_time);
    EXPECT_EQ(got.nonce, ack.nonce);
}

TEST(Aux, NackRoundTrip) {
    Nack nack;
    nack.stream_id = 9;
    nack.count = 3;
    nack.missing[0] = 10;
    nack.missing[1] = 15;
    nack.missing[2] = 21;

    uint8_t buf[kMaxAuxDatagramSize];
    size_t len = encode_nack(nack, buf, sizeof(buf));
    ASSERT_GT(len, 0u);

    Nack got;
    ASSERT_TRUE(decode_nack(buf, len, &got));
    EXPECT_EQ(got.stream_id, nack.stream_id);
    ASSERT_EQ(got.count, 3);
    EXPECT_EQ(got.missing[0], 10u);
    EXPECT_EQ(got.missing[1], 15u);
    EXPECT_EQ(got.missing[2], 21u);
}

TEST(Aux, EmptyNack) {
    Nack nack;
    nack.stream_id = 1;
    nack.count = 0;

    uint8_t buf[kMaxAuxDatagramSize];
    size_t len = encode_nack(nack, buf, sizeof(buf));
    ASSERT_GT(len, 0u);

    Nack got;
    ASSERT_TRUE(decode_nack(buf, len, &got));
    EXPECT_EQ(got.count, 0);
}

TEST(Aux, FullWindowNackFits) {
    Nack nack;
    nack.stream_id = 1;
    nack.count = kMaxWindow;
    for (uint16_t i = 0; i < kMaxWindow; ++i) nack.missing[i] = i * 3;

    uint8_t buf[kMaxAuxDatagramSize];
    size_t len = encode_nack(nack, buf, sizeof(buf));
    ASSERT_GT(len, 0u);
    EXPECT_LE(len, kMaxAuxDatagramSize);

    Nack got;
    ASSERT_TRUE(decode_nack(buf, len, &got));
    EXPECT_EQ(got.count, kMaxWindow);
    EXPECT_EQ(got.missing[kMaxWindow - 1], static_cast<uint64_t>((kMaxWindow - 1) * 3));
}

TEST(Aux, DecodeRejectsWrongType) {
    Ack ack;
    ack.stream_id = 1;
    uint8_t buf[kMaxAuxDatagramSize];
    size_t len = encode_ack(ack, buf, sizeof(buf));

    // An ACK datagram must not decode as a heartbeat.
    Heartbeat hb;
    EXPECT_FALSE(decode_heartbeat(buf, len, &hb));
}

TEST(Aux, StreamCloseRoundTrip) {
    for (uint8_t reason : {kStreamCloseFinished, kStreamCloseAborted}) {
        StreamClose sc;
        sc.stream_id = 3;
        sc.nonce = 0x0123456789ABCDEFULL;
        sc.reason = reason;

        uint8_t buf[64];
        const size_t len = encode_stream_close(sc, buf, sizeof(buf));
        ASSERT_GT(len, 0u);

        StreamClose got;
        ASSERT_TRUE(decode_stream_close(buf, len, &got));
        EXPECT_EQ(got.stream_id, sc.stream_id);
        EXPECT_EQ(got.nonce, sc.nonce);
        EXPECT_EQ(got.reason, reason);

        // Truncated, or a different message type, must not decode.
        EXPECT_FALSE(decode_stream_close(buf, len - 1, &got));
        Ack ack;
        EXPECT_FALSE(decode_ack(buf, len, &ack));
    }
}

TEST(Aux, ResumeRangesRoundTrip) {
    ResumeRanges rr;
    rr.stream_id = 2;
    rr.nonce = 0x1122334455667788ULL;
    rr.flags = kResumeFlagResumed;
    rr.part = 1;
    rr.parts = 3;
    rr.count = kMaxResumeRangesPerMsg;
    for (uint16_t i = 0; i < rr.count; ++i) rr.ranges[i] = {i * 131072ULL, 65536ULL + i};

    uint8_t buf[2048];
    const size_t len = encode_resume_ranges(rr, buf, sizeof(buf));
    ASSERT_GT(len, 0u);
    EXPECT_LT(len, 1200u) << "a full part must still fit a normal MTU";

    ResumeRanges got;
    ASSERT_TRUE(decode_resume_ranges(buf, len, &got));
    EXPECT_EQ(got.stream_id, rr.stream_id);
    EXPECT_EQ(got.nonce, rr.nonce);
    EXPECT_EQ(got.flags, rr.flags);
    EXPECT_EQ(got.part, rr.part);
    EXPECT_EQ(got.parts, rr.parts);
    ASSERT_EQ(got.count, rr.count);
    for (uint16_t i = 0; i < rr.count; ++i) {
        EXPECT_EQ(got.ranges[i].offset, rr.ranges[i].offset);
        EXPECT_EQ(got.ranges[i].length, rr.ranges[i].length);
    }
    EXPECT_FALSE(decode_resume_ranges(buf, len - 1, &got)) << "truncated";

    // A part index outside the declared count is refused both ways.
    rr.part = 3;
    EXPECT_EQ(encode_resume_ranges(rr, buf, sizeof(buf)), 0u);
}

TEST(Aux, DigestMessageRoundTrip) {
    DigestMessage dm;
    dm.stream_id = 0;
    dm.nonce = 42;
    for (size_t i = 0; i < kDigestLen; ++i) dm.digest[i] = static_cast<uint8_t>(i * 7);
    uint8_t buf[128];
    const size_t len = encode_digest(dm, buf, sizeof(buf));
    ASSERT_GT(len, 0u);
    DigestMessage got;
    ASSERT_TRUE(decode_digest(buf, len, &got));
    EXPECT_EQ(got.nonce, dm.nonce);
    EXPECT_EQ(0, std::memcmp(got.digest, dm.digest, kDigestLen));
    EXPECT_FALSE(decode_digest(buf, len - 1, &got));
}
