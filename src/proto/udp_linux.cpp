#include "fuse/proto/udp.hpp"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // recvmmsg
#endif

#include <linux/udp.h> // UDP_SEGMENT
#include <sys/socket.h>

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
// header — see its comment there). Linux-shaped: recvmmsg's own struct
// array plus the iovecs/addresses it scatters into.
struct BatchScratch {
    mmsghdr msgs[64]{};
    iovec iovs[64]{};
    sockaddr_storage addrs[64]{};
};
} // namespace

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

} // namespace fuse::proto
