#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "fuse/proto/block.hpp"
#include "fuse/proto/control.hpp"
#include "fuse/proto/udp.hpp"
#include "fuse/sdk.h"
#include "test_ports.hpp"

namespace {

// Each test takes its own port so cases can run back to back without
// tripping over a lingering socket.
uint16_t next_port() { return fuse_test::free_port_block(4); }

std::string make_payload(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>('a' + (i * 7 + (i >> 5)) % 26);
    return s;
}

struct Server {
    fuse_listener *l = nullptr;
    uint16_t port = 0;

    explicit Server(const char *psk = nullptr) {
        fuse_config cfg;
        fuse_config_init(&cfg);
        cfg.bind_address = "127.0.0.1";
        cfg.port = next_port();
        cfg.pre_shared_key = psk;
        cfg.timeout_ms = 5000;
        fuse_status err = FUSE_OK;
        l = fuse_listen(&cfg, &err);
        port = cfg.port;
    }
    ~Server() { fuse_listener_close(l); }
};

fuse_conn *client_connect(uint16_t port, const char *psk, fuse_status *err) {
    fuse_config cfg;
    fuse_config_init(&cfg);
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.pre_shared_key = psk;
    cfg.timeout_ms = 5000;
    return fuse_connect(&cfg, err);
}

} // namespace

TEST(Sdk, VersionAndLimits) {
    EXPECT_NE(fuse_version(), nullptr);
    EXPECT_GT(fuse_max_message(), 0u);
    EXPECT_STRNE(fuse_strerror(FUSE_OK), fuse_strerror(FUSE_ERR_TIMEOUT));
}

TEST(Sdk, ListenerReportsItsPort) {
    Server s;
    ASSERT_NE(s.l, nullptr);
    EXPECT_EQ(fuse_listener_port(s.l), s.port);
}

// The core socket pattern: accept on one side, connect on the other, send a
// message, read it back on the far end.
TEST(Sdk, ConnectAcceptAndRoundTrip) {
    Server s;
    ASSERT_NE(s.l, nullptr);

    fuse_conn *server_side = nullptr;
    std::thread acceptor([&] {
        fuse_status err = FUSE_OK;
        server_side = fuse_accept(s.l, 5000, &err);
    });

    fuse_status cerr = FUSE_OK;
    fuse_conn *client = client_connect(s.port, nullptr, &cerr);
    acceptor.join();

    ASSERT_NE(client, nullptr) << fuse_strerror(cerr);
    ASSERT_NE(server_side, nullptr);

    const std::string msg = "hello over fuse";
    ASSERT_EQ(fuse_send(client, msg.data(), msg.size()), FUSE_OK);

    char buf[256];
    size_t n = 0;
    ASSERT_EQ(fuse_recv(server_side, buf, sizeof(buf), &n, 5000), FUSE_OK);
    EXPECT_EQ(std::string(buf, n), msg);

    fuse_close(client);
    fuse_close(server_side);
}

// Message boundaries must be preserved: three sends must arrive as exactly
// three receives, not as one coalesced blob (the way a TCP stream would).
TEST(Sdk, PreservesMessageBoundaries) {
    Server s;
    fuse_conn *srv = nullptr;
    std::thread acceptor([&] { srv = fuse_accept(s.l, 5000, nullptr); });
    fuse_conn *cli = client_connect(s.port, nullptr, nullptr);
    acceptor.join();
    ASSERT_NE(cli, nullptr);
    ASSERT_NE(srv, nullptr);

    const char *msgs[3] = {"one", "two-two", "three-three-three"};
    for (const char *m : msgs) ASSERT_EQ(fuse_send(cli, m, std::strlen(m)), FUSE_OK);

    for (const char *m : msgs) {
        char buf[128];
        size_t n = 0;
        ASSERT_EQ(fuse_recv(srv, buf, sizeof(buf), &n, 5000), FUSE_OK);
        EXPECT_EQ(std::string(buf, n), std::string(m));
    }

    fuse_close(cli);
    fuse_close(srv);
}

TEST(Sdk, BidirectionalTraffic) {
    Server s;
    fuse_conn *srv = nullptr;
    std::thread acceptor([&] { srv = fuse_accept(s.l, 5000, nullptr); });
    fuse_conn *cli = client_connect(s.port, nullptr, nullptr);
    acceptor.join();
    ASSERT_NE(cli, nullptr);
    ASSERT_NE(srv, nullptr);

    // An echo exchange in both directions, several times over.
    std::thread echo([&] {
        for (int i = 0; i < 5; ++i) {
            char buf[256];
            size_t n = 0;
            if (fuse_recv(srv, buf, sizeof(buf), &n, 5000) != FUSE_OK) return;
            std::string reply = "echo:" + std::string(buf, n);
            fuse_send(srv, reply.data(), reply.size());
        }
    });

    for (int i = 0; i < 5; ++i) {
        const std::string out = "ping" + std::to_string(i);
        ASSERT_EQ(fuse_send(cli, out.data(), out.size()), FUSE_OK);
        char buf[256];
        size_t n = 0;
        ASSERT_EQ(fuse_recv(cli, buf, sizeof(buf), &n, 5000), FUSE_OK);
        EXPECT_EQ(std::string(buf, n), "echo:" + out);
    }
    echo.join();

    fuse_close(cli);
    fuse_close(srv);
}

// A Data block's offset is an unauthenticated wire value in plaintext mode
// (it is only covered by the AEAD AAD once a PSK is set), so a block
// claiming an offset near UINT64_MAX plus a small payload can make
// `offset + payload_len` wrap past zero under naive unsigned addition —
// small enough to pass a bounds check of the form "offset + len <= cap",
// after which reassembly would memcpy at the real, unwrapped offset: a
// heap out-of-bounds write reachable by anyone who can reach the
// connection's ephemeral port, no key needed.
TEST(Sdk, MaliciousOffsetIsRejectedNotWritten) {
    using namespace fuse::proto;
    Server s;
    fuse_conn *srv = nullptr;
    std::thread acceptor([&] { srv = fuse_accept(s.l, 5000, nullptr); });
    fuse_conn *cli = client_connect(s.port, nullptr, nullptr);
    acceptor.join();
    ASSERT_NE(cli, nullptr);
    ASSERT_NE(srv, nullptr);

    // The server answers HELLO from a fresh per-connection ephemeral
    // socket and the client adopts that as its peer — so `cli`'s peer
    // address *is* the server connection's own receiving port, the one
    // handle_data_locked() runs against.
    char addr[64] = {};
    uint16_t target_port = 0;
    ASSERT_EQ(fuse_conn_peer(cli, addr, sizeof(addr), &target_port), FUSE_OK);

    UdpSocket atk;
    ASSERT_TRUE(atk.open("127.0.0.1", 0));
    PeerAddr dst;
    ASSERT_TRUE(UdpSocket::resolve("127.0.0.1", target_port, &dst));

    // stream_id 0 is the client->server lane (kLaneClientToServer in
    // sdk.cpp) — an in-window seq_no so the receive window accepts it, an
    // offset designed to wrap.
    {
        BlockHeader hdr;
        hdr.stream_id = 0;
        hdr.seq_no = 1;
        hdr.flags = 0;
        hdr.payload_len = 8;
        hdr.offset = UINT64_MAX - 4;
        uint8_t body[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
        uint8_t dg[512];
        const size_t dn = encode_data_datagram(hdr, 0, body, dg, sizeof(dg));
        ASSERT_GT(dn, 0u);
        ASSERT_TRUE(atk.send_to(dg, dn, dst));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // The connection must still be healthy: a legitimate message sent
    // afterward arrives intact, proving the malicious block's payload was
    // dropped rather than written into the reassembly buffer (or worse).
    const std::string msg = "still healthy after the attack";
    ASSERT_EQ(fuse_send(cli, msg.data(), msg.size()), FUSE_OK);
    char buf[256];
    size_t n = 0;
    ASSERT_EQ(fuse_recv(srv, buf, sizeof(buf), &n, 5000), FUSE_OK);
    EXPECT_EQ(std::string(buf, n), msg);

    fuse_close(cli);
    fuse_close(srv);
}

// Larger than one block, and larger than the in-flight window, so this
// exercises fragmentation, the sliding window and reassembly.
TEST(Sdk, LargeMessageSpansManyBlocks) {
    Server s;
    fuse_conn *srv = nullptr;
    std::thread acceptor([&] { srv = fuse_accept(s.l, 5000, nullptr); });
    fuse_conn *cli = client_connect(s.port, nullptr, nullptr);
    acceptor.join();
    ASSERT_NE(cli, nullptr);
    ASSERT_NE(srv, nullptr);

    const std::string big = make_payload(512 * 1024); // ~437 blocks
    std::thread sender([&] { fuse_send(cli, big.data(), big.size()); });

    void *got = nullptr;
    size_t n = 0;
    ASSERT_EQ(fuse_recv_alloc(srv, &got, &n, 15000), FUSE_OK);
    sender.join();

    ASSERT_EQ(n, big.size());
    EXPECT_EQ(0, std::memcmp(got, big.data(), n));
    fuse_free(got);

    fuse_close(cli);
    fuse_close(srv);
}

TEST(Sdk, EmptyMessageIsDelivered) {
    Server s;
    fuse_conn *srv = nullptr;
    std::thread acceptor([&] { srv = fuse_accept(s.l, 5000, nullptr); });
    fuse_conn *cli = client_connect(s.port, nullptr, nullptr);
    acceptor.join();
    ASSERT_NE(cli, nullptr);

    ASSERT_EQ(fuse_send(cli, "", 0), FUSE_OK);
    char buf[16];
    size_t n = 123;
    ASSERT_EQ(fuse_recv(srv, buf, sizeof(buf), &n, 5000), FUSE_OK);
    EXPECT_EQ(n, 0u);

    fuse_close(cli);
    fuse_close(srv);
}

// A short buffer must report the size needed and leave the message queued,
// so the caller can allocate and retry without losing data.
TEST(Sdk, ShortBufferReportsSizeAndKeepsMessage) {
    Server s;
    fuse_conn *srv = nullptr;
    std::thread acceptor([&] { srv = fuse_accept(s.l, 5000, nullptr); });
    fuse_conn *cli = client_connect(s.port, nullptr, nullptr);
    acceptor.join();
    ASSERT_NE(cli, nullptr);

    const std::string msg = make_payload(4000);
    ASSERT_EQ(fuse_send(cli, msg.data(), msg.size()), FUSE_OK);

    char small[16];
    size_t need = 0;
    ASSERT_EQ(fuse_recv(srv, small, sizeof(small), &need, 5000), FUSE_ERR_BUFFER);
    EXPECT_EQ(need, msg.size());

    std::vector<char> big(need);
    size_t n = 0;
    ASSERT_EQ(fuse_recv(srv, big.data(), big.size(), &n, 5000), FUSE_OK)
        << "the message must survive a too-small read";
    EXPECT_EQ(std::string(big.data(), n), msg);

    fuse_close(cli);
    fuse_close(srv);
}

TEST(Sdk, RecvTimesOutWhenIdle) {
    Server s;
    fuse_conn *srv = nullptr;
    std::thread acceptor([&] { srv = fuse_accept(s.l, 5000, nullptr); });
    fuse_conn *cli = client_connect(s.port, nullptr, nullptr);
    acceptor.join();
    ASSERT_NE(srv, nullptr);

    char buf[16];
    size_t n = 0;
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(fuse_recv(srv, buf, sizeof(buf), &n, 300), FUSE_ERR_TIMEOUT);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 250);

    fuse_close(cli);
    fuse_close(srv);
}

TEST(Sdk, AcceptTimesOutWithNoClient) {
    Server s;
    ASSERT_NE(s.l, nullptr);
    fuse_status err = FUSE_OK;
    EXPECT_EQ(fuse_accept(s.l, 200, &err), nullptr);
    EXPECT_EQ(err, FUSE_ERR_TIMEOUT);
}

// Closing tells the peer, so its pending recv ends promptly rather than
// waiting out the full timeout.
TEST(Sdk, CloseNotifiesPeer) {
    Server s;
    fuse_conn *srv = nullptr;
    std::thread acceptor([&] { srv = fuse_accept(s.l, 5000, nullptr); });
    fuse_conn *cli = client_connect(s.port, nullptr, nullptr);
    acceptor.join();
    ASSERT_NE(cli, nullptr);
    ASSERT_NE(srv, nullptr);

    fuse_close(cli);

    char buf[16];
    size_t n = 0;
    EXPECT_EQ(fuse_recv(srv, buf, sizeof(buf), &n, 3000), FUSE_ERR_CLOSED);
    EXPECT_TRUE(fuse_conn_is_closed(srv));
    fuse_close(srv);
}

TEST(Sdk, PeerAddressAndStats) {
    Server s;
    fuse_conn *srv = nullptr;
    std::thread acceptor([&] { srv = fuse_accept(s.l, 5000, nullptr); });
    fuse_conn *cli = client_connect(s.port, nullptr, nullptr);
    acceptor.join();
    ASSERT_NE(cli, nullptr);
    ASSERT_NE(srv, nullptr);

    char addr[64] = {};
    uint16_t port = 0;
    ASSERT_EQ(fuse_conn_peer(srv, addr, sizeof(addr), &port), FUSE_OK);
    EXPECT_STREQ(addr, "127.0.0.1");
    EXPECT_NE(port, 0);

    const std::string m = "counted";
    ASSERT_EQ(fuse_send(cli, m.data(), m.size()), FUSE_OK);
    char buf[64];
    size_t n = 0;
    ASSERT_EQ(fuse_recv(srv, buf, sizeof(buf), &n, 5000), FUSE_OK);

    fuse_conn_stats cs{};
    fuse_conn_get_stats(cli, &cs);
    EXPECT_EQ(cs.messages_sent, 1u);
    EXPECT_EQ(cs.bytes_sent, m.size());
    fuse_conn_get_stats(srv, &cs);
    EXPECT_EQ(cs.messages_received, 1u);

    fuse_close(cli);
    fuse_close(srv);
}

// One listener serving several clients at once — each accepted connection
// has its own port, so they do not interfere.
TEST(Sdk, MultipleConcurrentClients) {
    Server s;
    ASSERT_NE(s.l, nullptr);
    constexpr int kClients = 3;

    std::vector<fuse_conn *> served;
    std::thread acceptor([&] {
        for (int i = 0; i < kClients; ++i) {
            fuse_conn *c = fuse_accept(s.l, 5000, nullptr);
            if (c) served.push_back(c);
        }
    });

    std::vector<fuse_conn *> clients;
    for (int i = 0; i < kClients; ++i) {
        fuse_conn *c = client_connect(s.port, nullptr, nullptr);
        ASSERT_NE(c, nullptr) << "client " << i;
        clients.push_back(c);
    }
    acceptor.join();
    ASSERT_EQ(served.size(), static_cast<size_t>(kClients));

    for (int i = 0; i < kClients; ++i) {
        const std::string m = "from-client-" + std::to_string(i);
        ASSERT_EQ(fuse_send(clients[i], m.data(), m.size()), FUSE_OK);
    }
    // Every served connection should get exactly one message.
    for (auto *c : served) {
        char buf[128];
        size_t n = 0;
        EXPECT_EQ(fuse_recv(c, buf, sizeof(buf), &n, 5000), FUSE_OK);
        EXPECT_GT(n, 0u);
    }

    for (auto *c : clients) fuse_close(c);
    for (auto *c : served) fuse_close(c);
}

TEST(Sdk, RejectsBadConfig) {
    fuse_status err = FUSE_OK;
    EXPECT_EQ(fuse_listen(nullptr, &err), nullptr);
    EXPECT_EQ(err, FUSE_ERR_CONFIG);

    fuse_config cfg;
    fuse_config_init(&cfg);
    cfg.port = 0; // a client needs a real destination port
    EXPECT_EQ(fuse_connect(&cfg, &err), nullptr);
    EXPECT_EQ(err, FUSE_ERR_CONFIG);
}

// fuse_close on one thread while another is blocked in fuse_recv must
// unblock that call with FUSE_ERR_CLOSED, not leave it referencing a
// connection that gets deleted out from under it. sdk.h documents one
// sender thread and one receiver thread per connection as the supported
// concurrency pattern, which makes "someone else closes while my recv is
// still blocked" an ordinary, expected interleaving, not a misuse case.
TEST(Sdk, CloseWhileRecvBlockedUnblocksCleanly) {
    Server s;
    fuse_conn *srv = nullptr;
    std::thread acceptor([&] { srv = fuse_accept(s.l, 5000, nullptr); });
    fuse_conn *cli = client_connect(s.port, nullptr, nullptr);
    acceptor.join();
    ASSERT_NE(cli, nullptr);
    ASSERT_NE(srv, nullptr);

    fuse_status rs = FUSE_OK;
    char buf[64];
    size_t n = 0;
    std::thread receiver([&] { rs = fuse_recv(cli, buf, sizeof(buf), &n, 5000); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    fuse_close(cli); // waits for the blocked recv to actually leave first
    receiver.join();

    EXPECT_EQ(rs, FUSE_ERR_CLOSED);

    fuse_close(srv);
}

// Two threads calling fuse_send concurrently on the same connection must
// not corrupt either message. Before send_busy serialization, wait_for's
// per-block room-wait released c->mu between blocks, so two concurrent
// sends could interleave their blocks onto the same tx_next_seq while each
// computed its own message-relative offset from 0 — the receiver's
// reassembly has no message id to disambiguate, silently corrupting both.
TEST(Sdk, ConcurrentSendsDoNotCorruptMessages) {
    Server s;
    fuse_conn *srv = nullptr;
    std::thread acceptor([&] { srv = fuse_accept(s.l, 5000, nullptr); });
    fuse_conn *cli = client_connect(s.port, nullptr, nullptr);
    acceptor.join();
    ASSERT_NE(cli, nullptr);
    ASSERT_NE(srv, nullptr);

    const std::string a(50000, 'A'); // spans many blocks (kBlockSize=1200)
    const std::string b(50000, 'B');

    fuse_status sa = FUSE_OK, sb = FUSE_OK;
    std::thread t1([&] { sa = fuse_send(cli, a.data(), a.size()); });
    std::thread t2([&] { sb = fuse_send(cli, b.data(), b.size()); });

    std::vector<std::string> got;
    for (int i = 0; i < 2; ++i) {
        void *p = nullptr;
        size_t n = 0;
        ASSERT_EQ(fuse_recv_alloc(srv, &p, &n, 5000), FUSE_OK);
        got.emplace_back(static_cast<char *>(p), n);
        fuse_free(p);
    }
    t1.join();
    t2.join();

    EXPECT_EQ(sa, FUSE_OK);
    EXPECT_EQ(sb, FUSE_OK);

    // send_busy only prevents interleaving, not a fixed arrival order.
    std::sort(got.begin(), got.end());
    std::vector<std::string> want = {a, b};
    std::sort(want.begin(), want.end());
    EXPECT_EQ(got, want) << "a concurrent send corrupted or interleaved a message";

    fuse_close(cli);
    fuse_close(srv);
}

// In plaintext a Close (or Ack/Nack) is only listened to from the peer's
// own address: a 2-byte Close from anywhere else used to end the
// connection for anyone who guessed its port.
TEST(Sdk, CloseFromAnotherAddressIsIgnored) {
    using namespace fuse::proto;
    Server s;
    fuse_conn *srv = nullptr;
    std::thread acceptor([&] { srv = fuse_accept(s.l, 5000, nullptr); });
    fuse_conn *cli = client_connect(s.port, nullptr, nullptr);
    acceptor.join();
    ASSERT_NE(cli, nullptr);
    ASSERT_NE(srv, nullptr);

    uint16_t target_port = 0;
    ASSERT_EQ(fuse_conn_peer(cli, nullptr, 0, &target_port), FUSE_OK);
    UdpSocket atk;
    ASSERT_TRUE(atk.open("127.0.0.1", 0));
    PeerAddr dst;
    ASSERT_TRUE(UdpSocket::resolve("127.0.0.1", target_port, &dst));
    const uint8_t bye[2] = {kProtocolVersion, static_cast<uint8_t>(MsgType::Close)};
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(atk.send_to(bye, sizeof(bye), dst));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(fuse_conn_is_closed(srv), 0);
    const std::string msg = "still open";
    ASSERT_EQ(fuse_send(cli, msg.data(), msg.size()), FUSE_OK);
    char buf[64];
    size_t n = 0;
    ASSERT_EQ(fuse_recv(srv, buf, sizeof(buf), &n, 5000), FUSE_OK);
    EXPECT_EQ(std::string(buf, n), msg);
    fuse_close(cli);
    fuse_close(srv);
}

// ROADMAP #12: a peer that vanishes without a Close is noticed. A fake
// server completes the handshake and then never sends again; the client's
// fuse_recv(-1) must give up instead of waiting forever.
TEST(Sdk, SilentPeerIsDetected) {
    using namespace fuse::proto;
    UdpSocket fake;
    const uint16_t port = next_port();
    ASSERT_TRUE(fake.open("127.0.0.1", port));
    std::thread server([&] {
        uint8_t in[256];
        size_t n = 0;
        PeerAddr from;
        timeval tv{5, 0};
        setsockopt(fake.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (!fake.recv_from(in, sizeof(in), &n, &from)) return;
        uint8_t ack[2 + 16 + 24] = {kProtocolVersion, static_cast<uint8_t>(MsgType::HelloAck)};
        fake.send_to(ack, sizeof(ack), from); // plaintext: zero salt, zero proof
    });

    fuse_config cfg;
    fuse_config_init(&cfg);
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.timeout_ms = 500; // the dead-peer interval is at least 3 s anyway
    fuse_status err = FUSE_OK;
    fuse_conn *cli = fuse_connect(&cfg, &err);
    server.join();
    ASSERT_NE(cli, nullptr) << fuse_strerror(err);

    const auto t0 = std::chrono::steady_clock::now();
    char buf[16];
    size_t n = 0;
    EXPECT_EQ(fuse_recv(cli, buf, sizeof(buf), &n, -1), FUSE_ERR_TIMEOUT);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
    EXPECT_GE(waited, 2500);
    EXPECT_LT(waited, 6000);
    EXPECT_NE(fuse_conn_is_closed(cli), 0);
    fuse_close(cli);
}

// An idle but healthy connection is kept alive by heartbeats: neither side
// declares the other lost while both are merely waiting.
TEST(Sdk, IdleConnectionStaysAlive) {
    Server s;
    fuse_conn *srv = nullptr;
    std::thread acceptor([&] { srv = fuse_accept(s.l, 5000, nullptr); });
    fuse_conn *cli = client_connect(s.port, nullptr, nullptr);
    acceptor.join();
    ASSERT_NE(cli, nullptr);
    ASSERT_NE(srv, nullptr);
    char buf[16];
    size_t n = 0;
    // Longer than the dead-peer interval (5 s timeout here).
    EXPECT_EQ(fuse_recv(srv, buf, sizeof(buf), &n, 6000), FUSE_ERR_TIMEOUT);
    EXPECT_EQ(fuse_conn_is_closed(srv), 0);
    EXPECT_EQ(fuse_conn_is_closed(cli), 0);
    const std::string msg = "after a long silence";
    ASSERT_EQ(fuse_send(cli, msg.data(), msg.size()), FUSE_OK);
    ASSERT_EQ(fuse_recv(srv, buf, sizeof(buf), &n, 5000), FUSE_ERR_BUFFER);
    fuse_close(cli);
    fuse_close(srv);
}

#if FUSE_PROTO_WITH_DTLS

TEST(Sdk, EncryptedRoundTrip) {
    ASSERT_TRUE(fuse_encryption_available());
    const char *psk = "a-key-both-ends-share";
    Server s(psk);
    ASSERT_NE(s.l, nullptr);

    fuse_conn *srv = nullptr;
    std::thread acceptor([&] { srv = fuse_accept(s.l, 5000, nullptr); });
    fuse_status cerr = FUSE_OK;
    fuse_conn *cli = client_connect(s.port, psk, &cerr);
    acceptor.join();

    ASSERT_NE(cli, nullptr) << fuse_strerror(cerr);
    ASSERT_NE(srv, nullptr);

    const std::string secret = make_payload(9000); // spans several blocks
    ASSERT_EQ(fuse_send(cli, secret.data(), secret.size()), FUSE_OK);

    void *got = nullptr;
    size_t n = 0;
    ASSERT_EQ(fuse_recv_alloc(srv, &got, &n, 5000), FUSE_OK);
    ASSERT_EQ(n, secret.size());
    EXPECT_EQ(0, std::memcmp(got, secret.data(), n));
    fuse_free(got);

    fuse_conn_stats cs{};
    fuse_conn_get_stats(srv, &cs);
    EXPECT_EQ(cs.auth_failures, 0u);

    fuse_close(cli);
    fuse_close(srv);
}

// A client with the wrong key must be refused at the handshake, not allowed
// to exchange garbage that only fails later.
TEST(Sdk, WrongKeyIsRejectedAtConnect) {
    Server s("server-key");
    ASSERT_NE(s.l, nullptr);

    std::thread acceptor([&] {
        fuse_conn *c = fuse_accept(s.l, 2000, nullptr);
        fuse_close(c);
    });

    fuse_status err = FUSE_OK;
    fuse_conn *cli = client_connect(s.port, "a-different-key", &err);
    acceptor.join();

    EXPECT_EQ(cli, nullptr) << "a mismatched pre-shared key must not connect";
    EXPECT_EQ(err, FUSE_ERR_AUTH);
    if (cli) fuse_close(cli);
}

// Under a PSK, control messages must be signed: an unsigned Close, Ack or
// Nack -- even one from the peer's own address -- changes nothing.
TEST(Sdk, UnsignedControlIsIgnoredUnderPsk) {
    using namespace fuse::proto;
    const char *psk = "control-signing-test-key";
    Server s(psk);
    fuse_conn *srv = nullptr;
    std::thread acceptor([&] { srv = fuse_accept(s.l, 5000, nullptr); });
    fuse_conn *cli = client_connect(s.port, psk, nullptr);
    acceptor.join();
    ASSERT_NE(cli, nullptr);
    ASSERT_NE(srv, nullptr);

    uint16_t target_port = 0;
    ASSERT_EQ(fuse_conn_peer(cli, nullptr, 0, &target_port), FUSE_OK);
    UdpSocket atk;
    ASSERT_TRUE(atk.open("127.0.0.1", 0));
    PeerAddr dst;
    ASSERT_TRUE(UdpSocket::resolve("127.0.0.1", target_port, &dst));
    const uint8_t bye[2] = {kProtocolVersion, static_cast<uint8_t>(MsgType::Close)};
    Ack ack;
    ack.stream_id = 0;
    ack.base_seq_no = 1000;
    uint8_t abuf[512];
    const size_t an = encode_ack(ack, abuf, sizeof(abuf));
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(atk.send_to(bye, sizeof(bye), dst));
        ASSERT_TRUE(atk.send_to(abuf, an, dst));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(fuse_conn_is_closed(srv), 0);

    const std::string msg = make_payload(5000);
    ASSERT_EQ(fuse_send(srv, msg.data(), msg.size()), FUSE_OK);
    void *got = nullptr;
    size_t n = 0;
    ASSERT_EQ(fuse_recv_alloc(cli, &got, &n, 5000), FUSE_OK);
    EXPECT_EQ(std::string(static_cast<char *>(got), n), msg);
    fuse_free(got);
    fuse_close(cli);
    fuse_close(srv);
}

// ROADMAP #28: a binary key may contain NUL bytes; every byte counts.
TEST(Sdk, BinaryPreSharedKeyWithNulBytes) {
    static const char key[] = {'k', '\0', 'e', 'y', '-', 'w', 'i', 't', 'h',
                               '-', 'n', 'u', 'l', '\0', '!', '!'};
    static const char other[] = {'k', '\0', 'X', 'X', 'X', 'X', 'X', 'X', 'X',
                                 'X', 'X', 'X', 'X', '\0', 'X', 'X'};
    for (const bool same : {true, false}) {
        fuse_config scfg;
        fuse_config_init(&scfg);
        scfg.bind_address = "127.0.0.1";
        scfg.port = next_port();
        scfg.pre_shared_key = key;
        scfg.pre_shared_key_len = sizeof(key);
        scfg.timeout_ms = 2000;
        fuse_listener *l = fuse_listen(&scfg, nullptr);
        ASSERT_NE(l, nullptr);
        fuse_conn *srv = nullptr;
        std::thread acceptor([&] { srv = fuse_accept(l, 2000, nullptr); });
        fuse_config ccfg;
        fuse_config_init(&ccfg);
        ccfg.host = "127.0.0.1";
        ccfg.port = scfg.port;
        ccfg.pre_shared_key = same ? key : other; // equal up to the first NUL
        ccfg.pre_shared_key_len = sizeof(key);
        ccfg.timeout_ms = 2000;
        fuse_status err = FUSE_OK;
        fuse_conn *cli = fuse_connect(&ccfg, &err);
        acceptor.join();
        if (same) {
            ASSERT_NE(cli, nullptr) << fuse_strerror(err);
            const std::string msg = "binary keys work";
            ASSERT_EQ(fuse_send(cli, msg.data(), msg.size()), FUSE_OK);
            char buf[64];
            size_t n = 0;
            ASSERT_EQ(fuse_recv(srv, buf, sizeof(buf), &n, 2000), FUSE_OK);
            EXPECT_EQ(std::string(buf, n), msg);
        } else {
            EXPECT_EQ(cli, nullptr);
            EXPECT_EQ(err, FUSE_ERR_AUTH);
        }
        fuse_close(cli);
        fuse_close(srv);
        fuse_listener_close(l);
    }
}

#endif // FUSE_PROTO_WITH_DTLS
