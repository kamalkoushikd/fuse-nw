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

UdpSocket::~UdpSocket() {
    close();
}

UdpSocket::UdpSocket(UdpSocket &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

UdpSocket &UdpSocket::operator=(UdpSocket &&other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

bool UdpSocket::open(const char *addr, uint16_t port) {
    close();

    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        return false;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    if (addr == nullptr || std::strcmp(addr, "0.0.0.0") == 0) {
        local.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, addr, &local.sin_addr) != 1) {
        close();
        return false;
    }

    if (bind(fd_, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
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
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (getsockname(fd_, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        return 0;
    }
    return ntohs(bound.sin_port);
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
    out->addr = sockaddr_in{};
    out->addr.sin_family = AF_INET;
    out->addr.sin_port = htons(port);
    out->len = sizeof(sockaddr_in);
    if (addr == nullptr || std::strcmp(addr, "0.0.0.0") == 0) {
        out->addr.sin_addr.s_addr = htonl(INADDR_ANY);
        return true;
    }
    return inet_pton(AF_INET, addr, &out->addr.sin_addr) == 1;
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
    msg.msg_name = const_cast<sockaddr_in *>(&dst.addr);
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
                          PeerAddr *first_src) {
    if (fd_ < 0 || max_msgs == 0 || slot_size == 0) {
        return -1;
    }
    // Cap the per-call batch so these stay stack-friendly.
    constexpr size_t kMaxBatch = 64;
    if (max_msgs > kMaxBatch) {
        max_msgs = kMaxBatch;
    }

    mmsghdr msgs[kMaxBatch]{};
    iovec iovs[kMaxBatch]{};
    sockaddr_in addrs[kMaxBatch]{};

    for (size_t i = 0; i < max_msgs; ++i) {
        iovs[i].iov_base = buf + i * slot_size;
        iovs[i].iov_len = slot_size;
        msgs[i].msg_hdr.msg_iov = &iovs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name = &addrs[i];
        msgs[i].msg_hdr.msg_namelen = sizeof(sockaddr_in);
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
    }
    if (first_src != nullptr && got > 0) {
        first_src->addr = addrs[0];
        first_src->len = msgs[0].msg_hdr.msg_namelen;
    }
    return got;
}

bool UdpSocket::recv_from(uint8_t *buf, size_t buf_cap, size_t *out_len, PeerAddr *src) {
    if (fd_ < 0) {
        return false;
    }
    sockaddr_in from{};
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
