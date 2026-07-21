#ifndef FUSE_PROTO_DTLS_HPP
#define FUSE_PROTO_DTLS_HPP

// Stage 7 optional forced encryption.
//
// Fuse's data plane is datagram-based, so the correct primitive is DTLS,
// not TLS: TLS assumes a reliable, ordered byte stream underneath it, which
// the block layer deliberately does not provide. One DTLS session secures
// the whole endpoint-to-endpoint link — every stream and the aux channel
// ride inside it — so the block header, registry, and bitmask/NACK logic
// above this layer are unchanged and never need to know whether the bytes
// beneath them are encrypted.
//
// Key material is a pre-shared key, not a certificate: both endpoints are
// already known to each other, so PSK avoids a PKI for no security gain.
//
// Encryption is OFF by default and is a *local configuration decision*, not
// something negotiated in-band — an in-band negotiation would invite a
// downgrade attack. Both peers must be started with the same setting; a
// mismatch fails closed rather than silently falling back to plaintext.
// When encryption is required, the DTLS handshake must complete before any
// SETUP or block traffic is attempted, so SETUP itself is inside the tunnel.
//
// Availability: compiled against wolfSSL only when the crypto backend is
// enabled. Check dtls_available() (or FUSE_PROTO_WITH_DTLS) — when it is
// false, requiring encryption fails closed instead of running in the clear.

#include <cstddef>
#include <cstdint>
#include <string>

#include "fuse/proto/udp.hpp"

#ifndef FUSE_PROTO_WITH_DTLS
#define FUSE_PROTO_WITH_DTLS 0
#endif

namespace fuse::proto {

enum class DtlsRole { Client, Server };

struct DtlsConfig {
    // Local policy. If true, the DTLS handshake must succeed before any
    // other traffic; failure aborts session start. If false, this whole
    // layer is bypassed and behavior is identical to Stages 0-6.
    bool encryption_required = false;

    DtlsRole role = DtlsRole::Client;
    std::string psk_identity = "fuse";
    std::string psk_key;  // raw pre-shared key bytes

    // DTLS retransmit backoff bounds, in seconds. These cap how long a
    // handshake keeps retrying before giving up — important because a
    // failure (a wrong pre-shared key, say) otherwise sits in the backoff
    // cycle rather than reporting promptly.
    int handshake_timeout_init_sec = 1;
    int handshake_timeout_max_sec = 4;
};

enum class DtlsStatus {
    Ok,
    Disabled,        // encryption not required; layer bypassed
    Unsupported,     // built without wolfSSL/DTLS support
    HandshakeFailed, // includes PSK mismatch
    IoError,
    NotConnected,
};

bool dtls_available();

// A DTLS session bound to a UdpSocket and a peer. All record I/O is routed
// through the supplied socket via wolfSSL custom I/O callbacks, so the
// datagrams that hit the wire are ordinary UDP carrying DTLS records.
class DtlsSession {
public:
    DtlsSession();
    ~DtlsSession();

    DtlsSession(const DtlsSession &) = delete;
    DtlsSession &operator=(const DtlsSession &) = delete;

    // Prepares the session. Returns Disabled (a benign no-op) when
    // encryption is not required, or Unsupported when required but this
    // build has no DTLS — the fail-closed case.
    DtlsStatus configure(const DtlsConfig &config, UdpSocket *socket, const PeerAddr &peer);

    // Drives the DTLS handshake to completion (blocking, bounded by the
    // socket's receive timeout). Returns Ok on success.
    DtlsStatus handshake();

    bool is_established() const { return established_; }
    bool encryption_active() const { return encryption_active_; }

    // Application data in/out. When encryption is not required these are
    // plain socket send/recv, so callers use one code path either way.
    DtlsStatus send(const uint8_t *data, size_t len);
    DtlsStatus recv(uint8_t *buf, size_t buf_cap, size_t *out_len);

    // Bytes actually handed to / taken from the network, for verifying that
    // what leaves the host is ciphertext rather than plaintext.
    uint64_t wire_bytes_sent() const { return wire_bytes_sent_; }
    const uint8_t *last_wire_datagram() const { return last_wire_.data; }
    size_t last_wire_datagram_len() const { return last_wire_.len; }

#if FUSE_PROTO_WITH_DTLS
    // Internal: the raw datagram I/O the DTLS record layer is wired to via
    // wolfSSL's custom I/O callbacks. Not part of the intended public API;
    // return values are wolfSSL CBIO codes.
    int wire_send(const uint8_t *buf, size_t len);
    int wire_recv(uint8_t *buf, size_t cap);
#endif

private:
    struct LastWire {
        uint8_t data[2048] = {};
        size_t len = 0;
    };

    DtlsConfig config_;
    UdpSocket *socket_ = nullptr;
    PeerAddr peer_{};
    bool established_ = false;
    bool encryption_active_ = false;
    uint64_t wire_bytes_sent_ = 0;
    LastWire last_wire_;

    void *ctx_ = nullptr; // WOLFSSL_CTX*
    void *ssl_ = nullptr; // WOLFSSL*
};

} // namespace fuse::proto

#endif // FUSE_PROTO_DTLS_HPP
