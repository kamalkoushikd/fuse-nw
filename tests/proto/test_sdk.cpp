#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "fuse/sdk.h"

namespace {

// Each test takes its own port so cases can run back to back without
// tripping over a lingering socket.
uint16_t next_port() {
    static std::atomic<uint16_t> p{38000};
    return p.fetch_add(4);
}

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

#endif // FUSE_PROTO_WITH_DTLS
