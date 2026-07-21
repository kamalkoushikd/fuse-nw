#include <gtest/gtest.h>

#include "fuse/proto/aux.hpp"

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

    uint8_t buf[kMaxAuxDatagramSize];
    size_t len = encode_stream_start(ss, buf, sizeof(buf));
    ASSERT_GT(len, 0u);

    StreamStart got;
    ASSERT_TRUE(decode_stream_start(buf, len, &got));
    EXPECT_EQ(got.stream_id, ss.stream_id);
    EXPECT_EQ(got.total_blocks, ss.total_blocks);
    EXPECT_EQ(got.block_size, ss.block_size);
    EXPECT_EQ(got.total_bytes, ss.total_bytes);
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
    ack.received_bitmask = 0b10110;
    ack.echoed_send_time = 55555;

    uint8_t buf[kMaxAuxDatagramSize];
    size_t len = encode_ack(ack, buf, sizeof(buf));
    ASSERT_GT(len, 0u);

    Ack got;
    ASSERT_TRUE(decode_ack(buf, len, &got));
    EXPECT_EQ(got.stream_id, ack.stream_id);
    EXPECT_EQ(got.base_seq_no, ack.base_seq_no);
    EXPECT_EQ(got.received_bitmask, ack.received_bitmask);
    EXPECT_EQ(got.echoed_send_time, ack.echoed_send_time);
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
