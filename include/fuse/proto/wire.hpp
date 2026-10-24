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
//
// v3 widened the receive window from a single uint64 bitmask (64 blocks) to
// a multi-word bitmask (kMaskWords words), and added a handshake nonce to
// StreamStart/Ack so a stale ACK from a prior session can't be mistaken for
// confirmation of a new one. Both are on-wire size changes, hence the bump.
//
// v4 added a server-chosen salt to HelloAck (sdk.cpp). The session key used
// to be derived from the client's salt alone, so replaying (or merely
// retrying) a HELLO before its first reply arrived could make two live
// connections derive the identical key while both restart their AEAD
// sequence numbers at 0 — an AES-GCM (key, nonce) collision. Mixing in a
// salt the server generates fresh for every HELLO it answers means no two
// connections ever share a key, regardless of what the client sends.
//
// v5 widened window_size (StreamConfig, SETUP) from one wire byte to two, to
// raise kMaxWindow from 255 to 1024 in-flight blocks per stream. A window
// that small was the ceiling on throughput over any link with real
// bandwidth-delay product; the wire field simply couldn't express more.
//
// v6 added stream_base_offset and file_total_bytes to StreamStart so a
// receiver can write each block straight to its final place in the output
// file as it arrives, instead of buffering every stream in memory and
// stitching them together at the end.
//
// v7 added resume: StreamStart carries a file_id, the receiver answers with
// ResumeRanges (what it still needs), and a resumed transfer ends with a
// whole-file FileDigest.
//
// v8 authenticates the control plane under a pre-shared key (every control
// datagram carries a ctr+GMAC trailer, see session_crypto.hpp ControlAuth),
// takes the retransmission bit out of the DATA AAD, rotates data keys per
// 2^22-block epoch, and adds StreamClose reason "auth failed" so a key
// mismatch is diagnosed at the handshake.
inline constexpr uint8_t  kProtocolVersion = 8;

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

// The received-set is tracked as a multi-word bitmask (kMaskWords x
// uint64), so a stream's window can go up to kMaxWindow in-flight blocks.
// window_size is negotiated as a two-byte wire field in SETUP (setup.cpp
// put_u16/get_u16, v5) — raising the cap further needs that field widened
// too, a separate wire change.
inline constexpr uint16_t kMaxWindow = 1024;
inline constexpr size_t   kMaskWords = (static_cast<size_t>(kMaxWindow) + 63) / 64;

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
    // Bulk-transfer (fuse/transfer.hpp) end-of-lane signal, either side to
    // the other: "finished" once every byte is acknowledged, or "aborted"
    // when a side gives up (cancelled, timed out). Without it the peer can
    // only learn the other end is gone by waiting out its own timeout.
    StreamClose    = 11,
    // Resume (v7). Receiver -> sender, answering StreamStart: the byte
    // ranges of this stream it still needs (all of it, for a fresh
    // transfer). Split across several datagrams when the list is long.
    ResumeRanges   = 12,
    // Resume (v7). Sender -> receiver at the end of a resumed transfer: a
    // digest of the whole file, so the receiver can verify that the bytes it
    // kept from an earlier session match. The receiver echoes it back as the
    // acknowledgement.
    FileDigest     = 13,
    // Address validation (v8). Receiver -> sender, answering a StreamStart
    // that did not carry a valid token: "prove you can receive at this
    // address by echoing this token". Never larger than the StreamStart it
    // answers, so a spoofed source address gains no amplification.
    Retry          = 14,
    // Path validation (v8), either direction: before a peer address is
    // replaced by a new source, the new source must echo a random challenge.
    PathChallenge  = 15,
    PathResponse   = 16,
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

// --- Extensions (v8, ROADMAP #24) ----------------------------------------
//
// Every field change used to need a kProtocolVersion bump, which breaks
// interop outright. StreamStart and SETUP_DATA now end in an extension area
//
//     [ext_len:2][ type:2 len:2 value:len ]*
//
// that later versions can add optional fields to without a version bump.
// Rules: a receiver ignores extension types it does not know, except
// *critical* ones (top bit set, type >= 0x8000), which it must refuse the
// whole message for. This version defines no extension types.
inline constexpr size_t   kMaxExtensionBytes = 256;
inline constexpr uint16_t kExtensionCritical = 0x8000;

struct Extension {
    uint16_t type = 0;
    uint16_t len = 0;
    const uint8_t *value = nullptr;
};

// Iterates an extension area: returns true and fills *ext with the entry at
// *off (advancing it), false at the end or on a malformed entry.
inline bool next_extension(const uint8_t *area, size_t area_len, size_t *off, Extension *ext);

// Appends one entry. Returns the new used length, or 0 if it doesn't fit.
inline size_t append_extension(uint8_t *area, size_t cap, size_t used, uint16_t type,
                               const uint8_t *value, uint16_t len);

// True if the area is well formed and holds no critical type this version
// doesn't understand (which is every critical type, for now).
inline bool extensions_acceptable(const uint8_t *area, size_t area_len);

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

inline size_t put_u32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
    }
    return 4;
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

inline size_t get_u32(const uint8_t *p, uint32_t *v) {
    uint32_t out = 0;
    for (int i = 0; i < 4; ++i) {
        out = (out << 8) | p[i];
    }
    *v = out;
    return 4;
}

inline size_t get_u64(const uint8_t *p, uint64_t *v) {
    uint64_t out = 0;
    for (int i = 0; i < 8; ++i) {
        out = (out << 8) | p[i];
    }
    *v = out;
    return 8;
}

inline bool next_extension(const uint8_t *area, size_t area_len, size_t *off, Extension *ext) {
    if (*off + 4 > area_len) return false;
    get_u16(area + *off, &ext->type);
    get_u16(area + *off + 2, &ext->len);
    if (ext->len > area_len - *off - 4) return false;
    ext->value = area + *off + 4;
    *off += 4 + ext->len;
    return true;
}

inline size_t append_extension(uint8_t *area, size_t cap, size_t used, uint16_t type,
                               const uint8_t *value, uint16_t len) {
    if (used > cap || cap - used < 4u + len) return 0;
    put_u16(area + used, type);
    put_u16(area + used + 2, len);
    for (uint16_t i = 0; i < len; ++i) area[used + 4 + i] = value[i];
    return used + 4 + len;
}

inline bool extensions_acceptable(const uint8_t *area, size_t area_len) {
    size_t off = 0;
    Extension ext;
    while (off < area_len) {
        if (!next_extension(area, area_len, &off, &ext)) return false; // malformed
        if (ext.type & kExtensionCritical) return false;               // not understood
    }
    return true;
}

} // namespace fuse::proto

#endif // FUSE_PROTO_WIRE_HPP
