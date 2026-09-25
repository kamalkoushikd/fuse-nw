#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/time.h>

#include <cstring>

#include "fuse/proto/udp.hpp"

using namespace fuse::proto;

namespace {

void set_recv_timeout(int fd, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// Sends one datagram from tx to rx's resolved address and returns the bytes
// rx received, or fails the test via ASSERT.
std::string round_trip(const char *bind_addr) {
    UdpSocket rx, tx;
    if (!rx.open(bind_addr, 0) || !tx.open(bind_addr, 0)) {
        return {}; // caller decides whether this means "unsupported here"
    }
    set_recv_timeout(rx.fd(), 1000);

    PeerAddr rx_addr;
    EXPECT_TRUE(UdpSocket::resolve(bind_addr, rx.local_port(), &rx_addr));

    const char msg[] = "hello over the wire";
    EXPECT_TRUE(tx.send_to(reinterpret_cast<const uint8_t *>(msg), sizeof(msg), rx_addr));

    uint8_t buf[64] = {};
    size_t got = 0;
    PeerAddr src;
    EXPECT_TRUE(rx.recv_from(buf, sizeof(buf), &got, &src));
    return std::string(reinterpret_cast<char *>(buf), got);
}

} // namespace

TEST(Udp, Ipv4LoopbackRoundTrip) {
    std::string got = round_trip("127.0.0.1");
    EXPECT_STREQ(got.c_str(), "hello over the wire");
}

TEST(Udp, Ipv6LoopbackRoundTrip) {
    UdpSocket probe;
    if (!probe.open("::1", 0)) {
        GTEST_SKIP() << "IPv6 loopback not available in this environment";
    }
    probe.close();

    std::string got = round_trip("::1");
    EXPECT_STREQ(got.c_str(), "hello over the wire");
}

TEST(Udp, PeerAddrEqualityIsFamilyAware) {
    PeerAddr a4, b4, c4, a6;
    ASSERT_TRUE(UdpSocket::resolve("127.0.0.1", 4000, &a4));
    ASSERT_TRUE(UdpSocket::resolve("127.0.0.1", 4000, &b4));
    ASSERT_TRUE(UdpSocket::resolve("127.0.0.1", 4001, &c4));
    EXPECT_TRUE(a4 == b4) << "same address+port, same family, must compare equal";
    EXPECT_FALSE(a4 == c4) << "different port must compare unequal";

    UdpSocket probe;
    if (probe.open("::1", 0)) {
        probe.close();
        ASSERT_TRUE(UdpSocket::resolve("::1", 4000, &a6));
        EXPECT_FALSE(a4 == a6) << "same port but different family must never compare equal";
    }
}

TEST(Udp, PeerToStringAndPortRoundTripIpv4) {
    PeerAddr a;
    ASSERT_TRUE(UdpSocket::resolve("192.0.2.7", 4242, &a));
    EXPECT_EQ(peer_port(a), 4242);
    char buf[64] = {};
    ASSERT_TRUE(peer_to_string(a, buf, sizeof(buf)));
    EXPECT_STREQ(buf, "192.0.2.7");
}

TEST(Udp, PeerToStringAndPortRoundTripIpv6) {
    PeerAddr a;
    ASSERT_TRUE(UdpSocket::resolve("::1", 4242, &a));
    EXPECT_EQ(peer_port(a), 4242);
    char buf[64] = {};
    ASSERT_TRUE(peer_to_string(a, buf, sizeof(buf)));
    EXPECT_STREQ(buf, "::1");
}

TEST(Udp, ResolveRejectsGarbageAddress) {
    PeerAddr a;
    EXPECT_FALSE(UdpSocket::resolve("not-an-address", 1234, &a));
}
