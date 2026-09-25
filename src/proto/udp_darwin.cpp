#include "fuse/proto/udp.hpp"

// macOS (Darwin) has no direct equivalent of Linux's UDP_SEGMENT (the
// kernel slicing one big buffer into wire-sized datagrams for you), and no
// header available in this build environment to verify the exact shape of
// Darwin's own batch-I/O syscalls (sendmsg_x/recvmsg_x, added for the same
// "many datagrams, one syscall" reason recvmmsg/sendmmsg exist on Linux).
// So this file is a plain, one-syscall-per-datagram loop: correct today on
// any Darwin version, at the cost of not amortising syscall overhead the
// way the Linux path does.
//
// ponytail: one syscall per datagram instead of one syscall per batch —
// upgrade to sendmsg_x/recvmsg_x once verified against a real macOS SDK.

#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <utility>

namespace fuse::proto {

namespace {
// The real type behind UdpSocket::batch_scratch_ (kept opaque in the public
// header — see its comment there). Empty: the loop below needs no
// kernel-facing scratch buffers, but delete requires a complete type, and
// batch_scratch_ is shared storage declared once for every platform.
struct BatchScratch {};
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
    // No GSO on this platform: every send_segmented call falls back, always.
    // gso_failed_ latches this after the first attempt so callers that check
    // gso_unavailable() (to stop offering GSO-sized batches) see it promptly.
    gso_failed_ = true;

    for (size_t off = 0; off < len; off += segment_size) {
        const size_t n = std::min<size_t>(segment_size, len - off);
        if (!send_to(buf + off, n, dst)) {
            return false;
        }
    }
    return true;
}

int UdpSocket::recv_batch(uint8_t *buf, size_t slot_size, size_t max_msgs, size_t *lens,
                          PeerAddr *srcs) {
    if (fd_ < 0 || max_msgs == 0 || slot_size == 0) {
        return -1;
    }

    int got = 0;
    for (size_t i = 0; i < max_msgs; ++i) {
        size_t n = 0;
        PeerAddr src;
        // The first recv is allowed to block (honoring whatever timeout the
        // caller set on this socket, exactly like Linux's MSG_WAITFORONE);
        // every subsequent one in this batch must not block the caller
        // waiting for a second datagram that may never come — MSG_DONTWAIT
        // (standard BSD, not Darwin-specific) opportunistically drains
        // whatever else already arrived instead.
        if (i == 0) {
            if (!recv_from(buf + i * slot_size, slot_size, &n, &src)) {
                return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
            }
        } else {
            // recvfrom needs *addrlen set to the buffer size going in; it
            // overwrites it with the actual sockaddr size on return.
            src.len = sizeof(src.addr);
            const ssize_t r = recvfrom(fd_, buf + i * slot_size, slot_size, MSG_DONTWAIT,
                                       reinterpret_cast<sockaddr *>(&src.addr), &src.len);
            if (r < 0) {
                break; // EAGAIN/EWOULDBLOCK: nothing more pending right now
            }
            n = static_cast<size_t>(r);
        }
        lens[i] = n;
        if (srcs != nullptr) {
            srcs[i] = src;
        }
        ++got;
    }
    return got;
}

} // namespace fuse::proto
