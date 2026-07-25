#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

#include "fuse/transfer.hpp"

namespace {

// Ports are picked per-test to avoid collisions between concurrently
// running cases; the range must be free for `lanes` consecutive ports.
uint16_t next_port() {
    static std::atomic<uint16_t> p{41000};
    return p.fetch_add(64);
}

std::vector<uint8_t> make_payload(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 31 + (i >> 8));
    return v;
}

// Runs a full transfer with the receiver started first, and returns both
// sides' status.
struct Outcome {
    fuse::TransferStatus send_status = fuse::TransferStatus::Incomplete;
    fuse::TransferStatus recv_status = fuse::TransferStatus::Incomplete;
    std::vector<uint8_t> received;
    fuse::TransferStats send_stats;
    fuse::TransferStats recv_stats;
};

Outcome round_trip(const std::vector<uint8_t> &payload, uint16_t lanes,
                   const std::string &psk = "", uint16_t block = 1200) {
    Outcome o;
    const uint16_t port = next_port();

    fuse::TransferConfig rx;
    rx.bind_address = "127.0.0.1";
    rx.base_port = port;
    rx.lanes = lanes;
    rx.pre_shared_key = psk;
    rx.block_size = block;
    rx.timeout_ms = 20000;

    fuse::TransferConfig tx = rx;
    tx.host = "127.0.0.1";

    std::thread receiver([&] {
        o.recv_status = fuse::receive_buffer(rx, &o.received, &o.recv_stats);
    });
    // Give the receiver time to bind before the sender's opening message.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    o.send_status = fuse::send_buffer(tx, payload.data(), payload.size(), &o.send_stats);
    receiver.join();
    return o;
}

} // namespace

TEST(Transfer, RoundTripsExactBytes) {
    const auto payload = make_payload(1 << 20); // 1 MiB
    Outcome o = round_trip(payload, 2);

    ASSERT_EQ(o.send_status, fuse::TransferStatus::Ok) << fuse::to_string(o.send_status);
    ASSERT_EQ(o.recv_status, fuse::TransferStatus::Ok) << fuse::to_string(o.recv_status);
    EXPECT_EQ(o.received, payload) << "received bytes must match the source exactly";
    EXPECT_EQ(o.recv_stats.bytes, payload.size());
}

TEST(Transfer, SingleLane) {
    const auto payload = make_payload(256 * 1024);
    Outcome o = round_trip(payload, 1);
    ASSERT_EQ(o.recv_status, fuse::TransferStatus::Ok);
    EXPECT_EQ(o.received, payload);
}

TEST(Transfer, ManyLanesSplitAndReassemble) {
    // With more lanes than a trivially-divisible size, shard boundaries do
    // not fall on clean multiples — the stitching must still be exact.
    const auto payload = make_payload(700003);
    Outcome o = round_trip(payload, 8);
    ASSERT_EQ(o.recv_status, fuse::TransferStatus::Ok);
    EXPECT_EQ(o.received, payload);
}

TEST(Transfer, PayloadSmallerThanOneBlock) {
    const auto payload = make_payload(17);
    Outcome o = round_trip(payload, 4);
    ASSERT_EQ(o.recv_status, fuse::TransferStatus::Ok);
    EXPECT_EQ(o.received, payload);
}

TEST(Transfer, ReportsUsefulStats) {
    const auto payload = make_payload(1 << 20);
    Outcome o = round_trip(payload, 2);
    ASSERT_EQ(o.recv_status, fuse::TransferStatus::Ok);
    EXPECT_EQ(o.send_stats.bytes, payload.size());
    EXPECT_GT(o.send_stats.final_block_size, 0);
    EXPECT_GE(o.recv_stats.throughput_mb_per_s(), 0.0);
}

#if FUSE_PROTO_WITH_DTLS

TEST(Transfer, EncryptedRoundTrip) {
    ASSERT_TRUE(fuse::encryption_available());
    const auto payload = make_payload(1 << 20);
    Outcome o = round_trip(payload, 4, "a-shared-secret-between-peers");

    ASSERT_EQ(o.send_status, fuse::TransferStatus::Ok) << fuse::to_string(o.send_status);
    ASSERT_EQ(o.recv_status, fuse::TransferStatus::Ok) << fuse::to_string(o.recv_status);
    EXPECT_EQ(o.received, payload);
    EXPECT_EQ(o.recv_stats.auth_failures, 0u);
}

// A receiver holding the wrong key must not produce plausible-looking data:
// every block fails authentication, so the transfer does not complete.
TEST(Transfer, WrongKeyDoesNotYieldData) {
    const auto payload = make_payload(128 * 1024);
    const uint16_t port = next_port();

    fuse::TransferConfig rx;
    rx.bind_address = "127.0.0.1";
    rx.base_port = port;
    rx.lanes = 2;
    rx.pre_shared_key = "receiver-key-which-is-wrong";
    rx.timeout_ms = 3000;

    fuse::TransferConfig tx = rx;
    tx.host = "127.0.0.1";
    tx.pre_shared_key = "sender-key";

    std::vector<uint8_t> got;
    fuse::TransferStatus rs = fuse::TransferStatus::Ok;
    std::thread receiver([&] { rs = fuse::receive_buffer(rx, &got, nullptr); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    fuse::send_buffer(tx, payload.data(), payload.size(), nullptr);
    receiver.join();

    EXPECT_NE(rs, fuse::TransferStatus::Ok) << "a mismatched key must not complete a transfer";
    EXPECT_NE(got, payload) << "no plaintext may be recovered with the wrong key";
}

#endif // FUSE_PROTO_WITH_DTLS

TEST(Transfer, RejectsBadConfiguration) {
    fuse::TransferConfig cfg;
    cfg.lanes = 0;
    const uint8_t byte = 0;
    EXPECT_EQ(fuse::send_buffer(cfg, &byte, 1, nullptr), fuse::TransferStatus::ConfigError);

    std::vector<uint8_t> out;
    EXPECT_EQ(fuse::receive_buffer(cfg, &out, nullptr), fuse::TransferStatus::ConfigError);
}

TEST(Transfer, StatusStringsAreDistinct) {
    // Error reporting is part of the API surface: each status needs a
    // message a developer can act on.
    EXPECT_STRNE(fuse::to_string(fuse::TransferStatus::Ok),
                 fuse::to_string(fuse::TransferStatus::Timeout));
    EXPECT_STRNE(fuse::to_string(fuse::TransferStatus::SocketError),
                 fuse::to_string(fuse::TransferStatus::ConfigError));
}
