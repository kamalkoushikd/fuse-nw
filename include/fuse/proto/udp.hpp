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
//
// Dual-stack: an address is IPv4 or IPv6 depending on what it parses as.
// open()/resolve() treat nullptr/"0.0.0.0" as the IPv4 wildcard (existing
// behavior, unchanged) and "::" as the IPv6 wildcard; any other literal is
// tried as IPv4 first, then IPv6. There is no dual-stack (v4-mapped-on-v6)
// socket — a bound address is exactly one family, matching what its callers
// already assume one UdpSocket instance means one local endpoint.

#include <cstddef>
#include <cstdint>
#include <netinet/in.h>
#include <sys/socket.h>

namespace fuse::proto {

// An opaque, comparable peer address (IPv4 or IPv6). Endpoints copy these
// around to remember where to send ACKs/NACKs without touching sockaddr
// directly — use peer_port()/peer_to_string() below for the rare caller
// that needs to report an address to a human.
struct PeerAddr {
    sockaddr_storage addr{};
    socklen_t        len = 0;

    bool operator==(const PeerAddr &o) const;
};

// The address's port in host byte order, or 0 if addr is empty/unrecognized.
uint16_t peer_port(const PeerAddr &p);

// Formats just the address (no port) into buf, e.g. "192.0.2.1" or
// "2001:db8::1". Returns false if buf is too small or the family is
// unrecognized.
bool peer_to_string(const PeerAddr &p, char *buf, size_t buf_cap);

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
    // its length in lens[i]. If `srcs` is non-null it must have room for
    // max_msgs entries; srcs[i] gets datagram i's source address — every
    // datagram, not just the batch's first, since a mid-batch source change
    // (e.g. the peer's NAT mapping just rotated) is exactly what a caller
    // doing address-migration tracking needs to see. Returns the number
    // received, 0 if none are pending, or -1 on error.
    int recv_batch(uint8_t *buf, size_t slot_size, size_t max_msgs, size_t *lens,
                   PeerAddr *srcs);

private:
    int fd_ = -1;
    bool gso_failed_ = false; // set once the kernel refuses UDP GSO here
    // Scratch space for recv_batch's kernel-facing structs (mmsghdr/iovec/
    // sockaddr_in), heap-allocated once per socket instead of ~5.6KB of
    // stack pushed and popped on every single call. Kept opaque here (a
    // raw pointer to a struct defined in udp.cpp) so this public header
    // doesn't need <sys/socket.h>'s mmsghdr — and the _GNU_SOURCE it
    // requires — visible to every file that includes udp.hpp.
    void *batch_scratch_ = nullptr;
};

} // namespace fuse::proto

#endif // FUSE_PROTO_UDP_HPP
