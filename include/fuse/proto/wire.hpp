#ifndef FUSE_PROTO_WIRE_HPP
#define FUSE_PROTO_WIRE_HPP

// Stage 1 wire foundation for the Fuse datagram protocol.
//
// Every datagram begins with a fixed 2-byte outer header
// (version, msg_type). This carries forward the version-gating and
// full-read/write discipline established by the Stage 0 stream framing:
// a receiver rejects any datagram whose version it does not understand
// before interpreting a single further byte, and all multi-byte fields
// are big-endian (network byte order) so two hosts of differing native
// endianness agree on the wire.

#include <cstddef>
#include <cstdint>

namespace fuse::proto {

// v2 added an explicit byte offset to the DATA block header. Without it a
// receiver must infer a block's position as seq_no * block_size, which
// silently breaks the moment block size varies within a stream — and
// adapting block size to link conditions is exactly what the sender does.
inline constexpr uint8_t  kProtocolVersion = 2;

// The default block payload (kDefaultPayloadSize) sits under a typical
// 1500-byte path MTU once the datagram's own headers are accounted for, so
// an ordinary block never provokes IP fragmentation.
//
// kMaxPayloadSize is the ceiling a sender may probe *up to* when the link
// proves it can carry larger blocks (a loopback or jumbo-frame path). It is
// deliberately not the default: exceeding path MTU on a normal network
// causes IP fragmentation, where losing one fragment destroys the whole
// datagram. Growth is earned by observed stability, never assumed.
inline constexpr uint16_t kDefaultPayloadSize = 1200;
inline constexpr uint16_t kMaxPayloadSize = 16384;

// The received-set is tracked as a single uint64 bitmask, so a stream's
// window can never exceed 64 in-flight blocks regardless of the
// application's requested window_size.
inline constexpr uint8_t  kMaxWindow = 64;

enum class MsgType : uint8_t {
    Data           = 0,  // a data-plane block (Stage 1)
    Heartbeat      = 1,  // liveness + highest seq sent (Stage 1.5)
    Ack            = 2,  // cumulative base + bitmask + echoed send time (Stage 1.5)
    Nack           = 3,  // explicit list of missing seq_nos (Stage 1.5)
    SetupData      = 4,  // SETUP payload, sender -> receiver (Stage 2)
    SetupHashReply = 5,  // receiver's computed hash, receiver -> sender (Stage 2)
    SetupFinAck    = 6,  // sender's confirmed hash, sender -> receiver (Stage 2)
    StreamStart    = 7,  // per-stream length preamble for a bulk stream (Stage 4.2)
    // Socket-style session control (fuse/sdk.h). A client HELLO lands on the
    // listener's well-known port; the server answers HELLO-ACK from a fresh
    // ephemeral socket, and the client adopts that reply's source address as
    // its peer. That is how one listen port serves many connections without
    // needing connection IDs — the same trick TFTP uses.
    Hello          = 8,
    HelloAck       = 9,
    Close          = 10, // graceful teardown so the peer's recv() ends promptly
};

// Block header flag bits (Stage 1.1).
inline constexpr uint8_t kFlagRetransmission = 0x01; // bit0
inline constexpr uint8_t kFlagLastBlock      = 0x02; // bit1

// Fixed sizes, in bytes, of each on-wire region.
inline constexpr size_t kOuterHeaderSize = 2;                 // version + msg_type
inline constexpr size_t kBlockHeaderSize = 21;                // stream_id + seq_no + flags + payload_len + offset
inline constexpr size_t kSendTimeSize    = 8;                 // send_time_ns carried in DATA for RTT echo
inline constexpr size_t kDataPrefixSize  = kOuterHeaderSize + kBlockHeaderSize + kSendTimeSize; // = 31
inline constexpr size_t kMaxDatagramSize = kDataPrefixSize + kMaxPayloadSize;

// --- Big-endian fixed-width encode/decode -------------------------------
//
// These are the only functions permitted to touch raw wire bytes; every
// message (de)serializer is built from them, so byte order lives in one
// place. Each returns the number of bytes advanced, keeping callers'
// cursor arithmetic uniform.

inline size_t put_u8(uint8_t *p, uint8_t v) {
    p[0] = v;
    return 1;
}

inline size_t put_u16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
    return 2;
}

inline size_t put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
    }
    return 8;
}

inline size_t get_u8(const uint8_t *p, uint8_t *v) {
    *v = p[0];
    return 1;
}

inline size_t get_u16(const uint8_t *p, uint16_t *v) {
    *v = static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
    return 2;
}

inline size_t get_u64(const uint8_t *p, uint64_t *v) {
    uint64_t out = 0;
    for (int i = 0; i < 8; ++i) {
        out = (out << 8) | p[i];
    }
    *v = out;
    return 8;
}

} // namespace fuse::proto

#endif // FUSE_PROTO_WIRE_HPP
