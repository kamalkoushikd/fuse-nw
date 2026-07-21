#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/time.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "fuse/proto/dtls.hpp"
#include "fuse/proto/udp.hpp"

using namespace fuse::proto;

namespace {

void set_recv_timeout(int fd, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

const std::string kPsk = "fuse-stage7-shared-secret";
const char *kPlaintext = "FUSE-STAGE7-PLAINTEXT-CANARY";

} // namespace

// Acceptance: with encryption_required=false on both sides, this layer is a
// transparent pass-through — provably a no-op, same standard as Stage 6.
TEST(Dtls, DisabledIsATransparentNoOp) {
    UdpSocket a, b;
    ASSERT_TRUE(a.open("127.0.0.1", 0));
    ASSERT_TRUE(b.open("127.0.0.1", 0));
    set_recv_timeout(b.fd(), 1000);

    PeerAddr b_addr;
    ASSERT_TRUE(UdpSocket::resolve("127.0.0.1", b.local_port(), &b_addr));

    DtlsConfig cfg;
    cfg.encryption_required = false;

    DtlsSession sess;
    EXPECT_EQ(sess.configure(cfg, &a, b_addr), DtlsStatus::Disabled);
    EXPECT_EQ(sess.handshake(), DtlsStatus::Disabled);
    EXPECT_FALSE(sess.encryption_active());

    // Data still flows, in the clear, exactly as in earlier stages.
    ASSERT_EQ(sess.send(reinterpret_cast<const uint8_t *>(kPlaintext), strlen(kPlaintext)),
              DtlsStatus::Ok);

    uint8_t buf[256];
    size_t got = 0;
    ASSERT_TRUE(b.recv_from(buf, sizeof(buf), &got, nullptr));
    EXPECT_EQ(std::string(reinterpret_cast<char *>(buf), got), kPlaintext);
}

#if FUSE_PROTO_WITH_DTLS

namespace {

struct Endpoint {
    UdpSocket sock;
    DtlsSession sess;
};

// Runs a client and server DTLS handshake on loopback with the given PSKs.
// Returns {client_ok, server_ok}.
struct HandshakeOutcome {
    bool client_ok = false;
    bool server_ok = false;
    std::string wire_sample; // bytes actually put on the wire by the client
    uint64_t client_wire_bytes = 0;
};

HandshakeOutcome run_dtls(const std::string &client_psk, const std::string &server_psk,
                          bool exchange_data = true) {
    HandshakeOutcome out;

    auto server = std::make_unique<Endpoint>();
    auto client = std::make_unique<Endpoint>();
    if (!server->sock.open("127.0.0.1", 0)) return out;
    if (!client->sock.open("127.0.0.1", 0)) return out;
    set_recv_timeout(server->sock.fd(), 2000);
    set_recv_timeout(client->sock.fd(), 2000);

    PeerAddr server_addr;
    UdpSocket::resolve("127.0.0.1", server->sock.local_port(), &server_addr);
    PeerAddr client_addr;
    UdpSocket::resolve("127.0.0.1", client->sock.local_port(), &client_addr);

    DtlsConfig scfg;
    scfg.encryption_required = true;
    scfg.role = DtlsRole::Server;
    scfg.psk_key = server_psk;
    scfg.handshake_timeout_init_sec = 1;
    scfg.handshake_timeout_max_sec = 1;

    DtlsConfig ccfg;
    ccfg.encryption_required = true;
    ccfg.role = DtlsRole::Client;
    ccfg.psk_key = client_psk;
    ccfg.handshake_timeout_init_sec = 1;
    ccfg.handshake_timeout_max_sec = 1;

    if (server->sess.configure(scfg, &server->sock, client_addr) != DtlsStatus::Ok) return out;
    if (client->sess.configure(ccfg, &client->sock, server_addr) != DtlsStatus::Ok) return out;

    std::atomic<bool> server_ok{false};
    std::thread server_thread([&] {
        if (server->sess.handshake() != DtlsStatus::Ok) return;
        server_ok.store(true);
        if (!exchange_data) return;
        uint8_t buf[512];
        size_t got = 0;
        server->sess.recv(buf, sizeof(buf), &got);
    });

    out.client_ok = (client->sess.handshake() == DtlsStatus::Ok);
    if (out.client_ok && exchange_data) {
        client->sess.send(reinterpret_cast<const uint8_t *>(kPlaintext), strlen(kPlaintext));
    }
    server_thread.join();
    out.server_ok = server_ok.load();

    out.client_wire_bytes = client->sess.wire_bytes_sent();
    out.wire_sample.assign(reinterpret_cast<const char *>(client->sess.last_wire_datagram()),
                           client->sess.last_wire_datagram_len());
    return out;
}

} // namespace

// Acceptance: with encryption required and matching PSKs on both sides, a
// session completes over the DTLS tunnel, and what leaves the host is
// ciphertext — the plaintext canary never appears on the wire.
TEST(Dtls, MatchingPskCompletesAndPutsCiphertextOnTheWire) {
    HandshakeOutcome r = run_dtls(kPsk, kPsk);
    ASSERT_TRUE(r.client_ok) << "client DTLS handshake must succeed with a matching PSK";
    ASSERT_TRUE(r.server_ok) << "server DTLS handshake must succeed with a matching PSK";

    EXPECT_GT(r.client_wire_bytes, 0u);
    ASSERT_FALSE(r.wire_sample.empty());
    EXPECT_EQ(r.wire_sample.find(kPlaintext), std::string::npos)
        << "plaintext must never appear in the datagram put on the wire";
}

// Acceptance: a PSK mismatch is rejected at the DTLS handshake, not
// discovered later as a garbled payload.
TEST(Dtls, WrongPskIsRejectedAtHandshake) {
    HandshakeOutcome r = run_dtls(kPsk, "a-completely-different-key", /*exchange_data=*/false);
    EXPECT_FALSE(r.client_ok && r.server_ok)
        << "a mismatched pre-shared key must fail the handshake";
}

#endif // FUSE_PROTO_WITH_DTLS

// Acceptance: mismatched policy fails closed. A peer that requires
// encryption never silently falls back to plaintext — if this build cannot
// provide DTLS, configure() refuses rather than proceeding in the clear.
TEST(Dtls, RequiringEncryptionNeverFallsBackToPlaintext) {
    UdpSocket sock;
    ASSERT_TRUE(sock.open("127.0.0.1", 0));
    PeerAddr peer;
    ASSERT_TRUE(UdpSocket::resolve("127.0.0.1", 9, &peer));

    DtlsConfig cfg;
    cfg.encryption_required = true;
    cfg.psk_key = kPsk;

    DtlsSession sess;
    DtlsStatus st = sess.configure(cfg, &sock, peer);
    if (!dtls_available()) {
        EXPECT_EQ(st, DtlsStatus::Unsupported) << "must fail closed without DTLS support";
        EXPECT_FALSE(sess.encryption_active());
    } else {
        EXPECT_EQ(st, DtlsStatus::Ok);
        EXPECT_TRUE(sess.encryption_active());
    }
    // Either way, a session that requires encryption is never in a state
    // where it would send application data unencrypted.
    EXPECT_FALSE(sess.is_established());
}

TEST(Dtls, MissingPskIsRejectedWhenEncryptionRequired) {
    UdpSocket sock;
    ASSERT_TRUE(sock.open("127.0.0.1", 0));
    PeerAddr peer;
    ASSERT_TRUE(UdpSocket::resolve("127.0.0.1", 9, &peer));

    DtlsConfig cfg;
    cfg.encryption_required = true;
    cfg.psk_key = ""; // no key supplied

    DtlsSession sess;
    DtlsStatus st = sess.configure(cfg, &sock, peer);
    EXPECT_NE(st, DtlsStatus::Ok) << "an empty PSK must not yield a usable encrypted session";
    EXPECT_FALSE(sess.is_established());
}
