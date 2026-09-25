#include "fuse/proto/udp.hpp"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // recvmmsg
#endif

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/udp.h> // UDP_SEGMENT
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <new>
#include <utility>

#ifndef SOL_UDP
#define SOL_UDP 17
#endif

// UDP_SEGMENT (generic segmentation offload) arrived in Linux 4.18. Building
// inside an older sysroot — a manylinux container, say — can miss the
// definition even though the kernel the wheel eventually runs on supports it.
// The value is fixed ABI, so defining it keeps the build portable; a kernel
// that genuinely lacks GSO just fails the setsockopt/cmsg and the sender
// falls back to unbatched sends.
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

namespace fuse::proto {

namespace {
// The real type behind UdpSocket::batch_scratch_ (kept opaque in the public
// header — see its comment there).
struct BatchScratch {
    mmsghdr msgs[64]{};
    iovec iovs[64]{};
    sockaddr_storage addrs[64]{};
};

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

UdpSocket::~UdpSocket() {
    close();
    delete static_cast<BatchScratch *>(batch_scratch_);
    batch_scratch_ = nullptr;
}

UdpSocket::UdpSocket(UdpSocket &&other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      gso_failed_(std::exchange(other.gso_failed_, false)),
      batch_scratch_(std::exchange(other.batch_scratch_, nullptr)) {}

UdpSocket &UdpSocket::operator=(UdpSocket &&other) noexcept {
    if (this != &other) {
        close();
        delete static_cast<BatchScratch *>(batch_scratch_);
        fd_ = std::exchange(other.fd_, -1);
        gso_failed_ = std::exchange(other.gso_failed_, false);
        batch_scratch_ = std::exchange(other.batch_scratch_, nullptr);
    }
    return *this;
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

bool UdpSocket::send_segmented(const uint8_t *buf, size_t len, uint16_t segment_size,
                               const PeerAddr &dst) {
    if (fd_ < 0 || len == 0 || segment_size == 0) {
        return false;
    }
    if (gso_failed_) {
        return false; // already known unsupported on this socket
    }
    if (len <= segment_size) {
        // Single datagram: GSO buys nothing and some kernels reject a
        // segment size >= the payload.
        return send_to(buf, len, dst);
    }

    iovec iov{};
    iov.iov_base = const_cast<uint8_t *>(buf);
    iov.iov_len = len;

    // UDP_SEGMENT travels as a control message so the size can change per
    // send without a setsockopt round trip.
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(uint16_t))] = {};

    msghdr msg{};
    msg.msg_name = const_cast<sockaddr_storage *>(&dst.addr);
    msg.msg_namelen = dst.len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_UDP;
    cm->cmsg_type = UDP_SEGMENT;
    cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    std::memcpy(CMSG_DATA(cm), &segment_size, sizeof(segment_size));

    ssize_t sent = sendmsg(fd_, &msg, 0);
    if (sent < 0) {
        // EIO/ENOTSUP/EINVAL here means this route or kernel cannot do GSO.
        // Latch it so the caller falls back once rather than every send.
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS) {
            gso_failed_ = true;
        }
        return false;
    }
    return static_cast<size_t>(sent) == len;
}

int UdpSocket::recv_batch(uint8_t *buf, size_t slot_size, size_t max_msgs, size_t *lens,
                          PeerAddr *srcs) {
    if (fd_ < 0 || max_msgs == 0 || slot_size == 0) {
        return -1;
    }
    // Cap the per-call batch to what BatchScratch holds.
    constexpr size_t kMaxBatch = 64;
    if (max_msgs > kMaxBatch) {
        max_msgs = kMaxBatch;
    }

    if (batch_scratch_ == nullptr) {
        batch_scratch_ = new (std::nothrow) BatchScratch();
        if (batch_scratch_ == nullptr) {
            return -1;
        }
    }
    auto *scratch = static_cast<BatchScratch *>(batch_scratch_);
    mmsghdr *msgs = scratch->msgs;
    iovec *iovs = scratch->iovs;
    sockaddr_storage *addrs = scratch->addrs;

    for (size_t i = 0; i < max_msgs; ++i) {
        iovs[i].iov_base = buf + i * slot_size;
        iovs[i].iov_len = slot_size;
        msgs[i].msg_hdr.msg_iov = &iovs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name = &addrs[i];
        msgs[i].msg_hdr.msg_namelen = sizeof(sockaddr_storage);
    }

    // MSG_WAITFORONE is essential, not an optimisation: without it recvmmsg
    // blocks until the *entire* batch is filled (or the socket timeout
    // fires). A sender that emits a window and then waits for an ACK would
    // deadlock against a receiver waiting for more datagrams to fill its
    // batch, turning every round trip into a timeout.
    int got = recvmmsg(fd_, msgs, static_cast<unsigned>(max_msgs), MSG_WAITFORONE, nullptr);
    if (got < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }
    for (int i = 0; i < got; ++i) {
        lens[i] = msgs[i].msg_len;
        if (srcs != nullptr) {
            srcs[i].addr = addrs[i];
            srcs[i].len = msgs[i].msg_hdr.msg_namelen;
        }
    }
    return got;
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
