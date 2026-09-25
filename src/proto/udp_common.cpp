#include "fuse/proto/udp.hpp"

// The socket operations here are plain POSIX/BSD sockets (socket, bind,
// sendto, recvfrom, getsockname, inet_pton/inet_ntop) — identical on Linux
// and macOS, and indifferent to CPU architecture (x86_64, arm64, ...). This
// file is compiled on every platform fuse_proto supports.
//
// The two operations that are NOT here — send_segmented (batched send) and
// recv_batch (batched receive) — are the only parts of UdpSocket that rely
// on OS-specific batch-I/O syscalls (Linux: sendmmsg/recvmmsg/UDP_SEGMENT).
// Those live in udp_linux.cpp / udp_darwin.cpp, selected by CMakeLists.txt
// per CMAKE_SYSTEM_NAME, along with UdpSocket's destructor and move
// operations (which own batch_scratch_, a type private to those files).

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace fuse::proto {

namespace {
// Shared by open() (binds a local address) and resolve() (builds a peer
// address): nullptr/"0.0.0.0" is the IPv4 wildcard, "::" the IPv6 wildcard,
// and anything else is tried as an IPv4 literal, then an IPv6 one.
bool parse_sockaddr(const char *addr, uint16_t port, sockaddr_storage *out, socklen_t *out_len) {
    std::memset(out, 0, sizeof(*out));
    if (addr == nullptr || std::strcmp(addr, "0.0.0.0") == 0) {
        auto *v4 = reinterpret_cast<sockaddr_in *>(out);
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        v4->sin_addr.s_addr = htonl(INADDR_ANY);
        *out_len = sizeof(sockaddr_in);
        return true;
    }
    if (std::strcmp(addr, "::") == 0) {
        auto *v6 = reinterpret_cast<sockaddr_in6 *>(out);
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        v6->sin6_addr = in6addr_any;
        *out_len = sizeof(sockaddr_in6);
        return true;
    }
    auto *v4 = reinterpret_cast<sockaddr_in *>(out);
    if (inet_pton(AF_INET, addr, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        *out_len = sizeof(sockaddr_in);
        return true;
    }
    std::memset(out, 0, sizeof(*out));
    auto *v6 = reinterpret_cast<sockaddr_in6 *>(out);
    if (inet_pton(AF_INET6, addr, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        *out_len = sizeof(sockaddr_in6);
        return true;
    }
    return false;
}
} // namespace

bool PeerAddr::operator==(const PeerAddr &o) const {
    if (addr.ss_family != o.addr.ss_family) {
        return false;
    }
    if (addr.ss_family == AF_INET) {
        const auto &a = reinterpret_cast<const sockaddr_in &>(addr);
        const auto &b = reinterpret_cast<const sockaddr_in &>(o.addr);
        return a.sin_port == b.sin_port && a.sin_addr.s_addr == b.sin_addr.s_addr;
    }
    if (addr.ss_family == AF_INET6) {
        const auto &a = reinterpret_cast<const sockaddr_in6 &>(addr);
        const auto &b = reinterpret_cast<const sockaddr_in6 &>(o.addr);
        return a.sin6_port == b.sin6_port &&
               std::memcmp(&a.sin6_addr, &b.sin6_addr, sizeof(a.sin6_addr)) == 0;
    }
    return false;
}

uint16_t peer_port(const PeerAddr &p) {
    if (p.addr.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in &>(p.addr).sin_port);
    }
    if (p.addr.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6 &>(p.addr).sin6_port);
    }
    return 0;
}

bool peer_to_string(const PeerAddr &p, char *buf, size_t buf_cap) {
    if (p.addr.ss_family == AF_INET) {
        return inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in &>(p.addr).sin_addr, buf,
                         static_cast<socklen_t>(buf_cap)) != nullptr;
    }
    if (p.addr.ss_family == AF_INET6) {
        return inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6 &>(p.addr).sin6_addr, buf,
                         static_cast<socklen_t>(buf_cap)) != nullptr;
    }
    return false;
}

bool UdpSocket::open(const char *addr, uint16_t port) {
    close();

    sockaddr_storage local{};
    socklen_t local_len = 0;
    if (!parse_sockaddr(addr, port, &local, &local_len)) {
        return false;
    }

    fd_ = socket(local.ss_family, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        return false;
    }

    if (bind(fd_, reinterpret_cast<sockaddr *>(&local), local_len) != 0) {
        close();
        return false;
    }
    return true;
}

void UdpSocket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

uint16_t UdpSocket::local_port() const {
    if (fd_ < 0) {
        return 0;
    }
    PeerAddr bound;
    bound.len = sizeof(bound.addr);
    if (getsockname(fd_, reinterpret_cast<sockaddr *>(&bound.addr), &bound.len) != 0) {
        return 0;
    }
    return peer_port(bound);
}

bool UdpSocket::set_nonblocking(bool nonblocking) {
    if (fd_ < 0) {
        return false;
    }
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd_, F_SETFL, flags) == 0;
}

bool UdpSocket::resolve(const char *addr, uint16_t port, PeerAddr *out) {
    return parse_sockaddr(addr, port, &out->addr, &out->len);
}

bool UdpSocket::send_to(const uint8_t *data, size_t len, const PeerAddr &dst) {
    if (fd_ < 0) {
        return false;
    }
    ssize_t sent = sendto(fd_, data, len, 0,
                          reinterpret_cast<const sockaddr *>(&dst.addr), dst.len);
    return sent >= 0 && static_cast<size_t>(sent) == len;
}

bool UdpSocket::recv_from(uint8_t *buf, size_t buf_cap, size_t *out_len, PeerAddr *src) {
    if (fd_ < 0) {
        return false;
    }
    sockaddr_storage from{};
    socklen_t from_len = sizeof(from);
    ssize_t got = recvfrom(fd_, buf, buf_cap, 0,
                           reinterpret_cast<sockaddr *>(&from), &from_len);
    if (got < 0) {
        return false;
    }
    *out_len = static_cast<size_t>(got);
    if (src != nullptr) {
        src->addr = from;
        src->len = from_len;
    }
    return true;
}

} // namespace fuse::proto
