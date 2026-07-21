#ifndef FUSE_PROTO_UDP_HPP
#define FUSE_PROTO_UDP_HPP

// Stage 1 datagram transport: a thin wrapper over a Linux UDP socket.
//
// Fuse's data plane rides directly on independently-addressed datagrams,
// not on TCP's connection/ordering semantics, so there is no stream to
// reassemble here — a datagram is delivered whole or not at all. The
// wrapper's job is only the send/recv discipline: a receive buffer sized
// to hold the largest possible datagram (so the kernel never truncates a
// block), and the peer address plumbing the endpoints need to reply.

#include <cstddef>
#include <cstdint>
#include <netinet/in.h>

namespace fuse::proto {

// An opaque, comparable peer address (IPv4). Endpoints copy these around
// to remember where to send ACKs/NACKs without touching sockaddr directly.
struct PeerAddr {
    sockaddr_in addr{};
    socklen_t   len = sizeof(sockaddr_in);
};

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket &) = delete;
    UdpSocket &operator=(const UdpSocket &) = delete;
    UdpSocket(UdpSocket &&other) noexcept;
    UdpSocket &operator=(UdpSocket &&other) noexcept;

    // Opens a UDP socket and binds it. addr may be nullptr/"0.0.0.0" for
    // INADDR_ANY; port 0 lets the kernel choose. Returns false on failure.
    bool open(const char *addr, uint16_t port);
    void close();

    bool is_open() const { return fd_ >= 0; }
    int  fd() const { return fd_; }

    // The bound local port (useful when the kernel chose an ephemeral one).
    uint16_t local_port() const;

    bool set_nonblocking(bool nonblocking);

    // Resolves "a.b.c.d":port into a PeerAddr for send_to. Returns false
    // on a malformed address.
    static bool resolve(const char *addr, uint16_t port, PeerAddr *out);

    // Sends one datagram. Returns true iff the whole datagram was handed
    // to the kernel in a single sendto.
    bool send_to(const uint8_t *data, size_t len, const PeerAddr &dst);

    // Receives at most one datagram into buf (which must be at least
    // kMaxDatagramSize to avoid truncation). On success sets *out_len and,
    // if src is non-null, the sender's address. Returns false on error or,
    // for a non-blocking socket with nothing pending, with errno left as
    // EAGAIN/EWOULDBLOCK for the caller to distinguish.
    bool recv_from(uint8_t *buf, size_t buf_cap, size_t *out_len, PeerAddr *src);

    // --- Batched I/O -----------------------------------------------------
    //
    // One syscall per datagram puts a hard ceiling on throughput that has
    // nothing to do with bandwidth (measured at ~213k datagrams/sec on a
    // modern host). These two amortise that cost over many datagrams.

    // Sends a buffer of back-to-back datagrams, each exactly `segment_size`
    // bytes (the final one may be shorter), in a SINGLE syscall using UDP
    // GSO: the kernel does the segmentation, so what reaches the wire is
    // still ordinary MTU-sized datagrams. Returns false if the kernel
    // rejects GSO, in which case the caller should fall back to send_to.
    bool send_segmented(const uint8_t *buf, size_t len, uint16_t segment_size,
                        const PeerAddr &dst);

    // True once send_segmented has failed on this socket, so callers can
    // stop retrying a path the kernel/route does not support.
    bool gso_unavailable() const { return gso_failed_; }

    // Receives up to `max_msgs` datagrams in one syscall. `buf` must hold
    // max_msgs * slot_size bytes; datagram i lands at buf + i*slot_size with
    // its length in lens[i]. Returns the number received, 0 if none are
    // pending, or -1 on error.
    int recv_batch(uint8_t *buf, size_t slot_size, size_t max_msgs, size_t *lens,
                   PeerAddr *first_src);

private:
    int fd_ = -1;
    bool gso_failed_ = false; // set once the kernel refuses UDP GSO here
};

} // namespace fuse::proto

#endif // FUSE_PROTO_UDP_HPP
