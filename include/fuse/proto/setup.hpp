#ifndef FUSE_PROTO_SETUP_HPP
#define FUSE_PROTO_SETUP_HPP

// Stage 2 SETUP handshake.
//
// Before any data-plane traffic, the two endpoints agree once on the
// stream/worker topology for the session. This is a generic
// stream-configuration exchange — nothing about it assumes a particular
// workload. Because no other reliability machinery exists yet, SETUP
// carries its own: a three-way DATA -> HASH-REPLY -> FINACK exchange in
// which the receiver echoes its independently-computed hash of what it
// got (a corrupted hash simply fails to match, unlike a corruptible
// boolean "ok"), and each side retransmits on silence up to a cap.
//
// Wire formats (each prefixed by the 2-byte outer header from wire.hpp):
//   SetupData      : [protocol_version:1][num_workers:2][num_streams:2]
//                    [ stream_id:2 worker_id:2 flags:1 block_size:2 window_size:1 ]*num_streams
//   SetupHashReply : [hash:8]
//   SetupFinAck    : [hash:8]

#include <cstddef>
#include <cstdint>

#include "fuse/proto/wire.hpp"

namespace fuse::proto {

// Per-stream flag bits negotiated at SETUP; their behavior is defined in
// Stage 4, but the wire field and constants live here since SETUP carries
// them.
inline constexpr uint8_t kStreamFlagLossless = 0x01; // bit0
inline constexpr uint8_t kStreamFlagOrdered  = 0x02; // bit1
inline constexpr uint8_t kStreamFlagCoalesce = 0x04; // bit2

inline constexpr uint16_t kMaxStreams = 64;

struct StreamConfig {
    uint16_t stream_id    = 0;
    uint16_t worker_id    = 0;   // explicit assignment, never a computed formula
    uint8_t  stream_flags = 0;
    uint16_t block_size   = 0;
    uint8_t  window_size  = 0;

    bool operator==(const StreamConfig &o) const {
        return stream_id == o.stream_id && worker_id == o.worker_id &&
               stream_flags == o.stream_flags && block_size == o.block_size &&
               window_size == o.window_size;
    }
};

struct SetupPayload {
    uint8_t  protocol_version = kProtocolVersion;
    uint16_t num_workers = 0;
    uint16_t num_streams = 0;
    StreamConfig streams[kMaxStreams] = {};

    bool operator==(const SetupPayload &o) const {
        if (protocol_version != o.protocol_version || num_workers != o.num_workers ||
            num_streams != o.num_streams) {
            return false;
        }
        for (uint16_t i = 0; i < num_streams; ++i) {
            if (!(streams[i] == o.streams[i])) return false;
        }
        return true;
    }
};

// Largest a SETUP datagram can be: outer + fixed prefix + full stream table.
inline constexpr size_t kMaxSetupDatagramSize =
    kOuterHeaderSize + 1 + 2 + 2 + static_cast<size_t>(kMaxStreams) * 8;

// Serializes just the SETUP payload body (everything after the outer
// header) into `out`. Returns the byte count, or 0 on overflow / an
// out-of-range num_streams. Exposed so both sides hash the exact same
// canonical byte range.
size_t serialize_setup_payload(const SetupPayload &p, uint8_t *out, size_t out_cap);

size_t encode_setup_data(const SetupPayload &p, uint8_t *out, size_t out_cap);

// Decodes a SetupData datagram. On success fills *p and, if raw_body /
// raw_body_len are non-null, points them at the payload-body byte range
// (for hashing exactly what was received). Returns false on
// version/type/length errors or num_streams > kMaxStreams.
bool decode_setup_data(const uint8_t *in, size_t in_len, SetupPayload *p,
                       const uint8_t **raw_body, size_t *raw_body_len);

size_t encode_setup_hash(MsgType type, uint64_t hash, uint8_t *out, size_t out_cap);
bool   decode_setup_hash(MsgType expected, const uint8_t *in, size_t in_len, uint64_t *hash);

// --- Handshake state machines -------------------------------------------
//
// Both are pure logic: they consume incoming datagrams and a clock, and
// emit outgoing datagrams into a caller-provided buffer. The caller owns
// the socket I/O and the timer cadence (calling on_timeout periodically).
// This keeps the handshake testable with a virtual clock and injected
// loss, exactly as the acceptance criteria require.

inline constexpr uint64_t kDefaultSetupTimeoutNs = 50'000'000; // 50 ms
inline constexpr uint8_t  kDefaultSetupMaxRetries = 5;

// The endpoint that offers the configuration (sends DATA, sends FINACK).
class SetupInitiator {
public:
    explicit SetupInitiator(const SetupPayload &payload,
                            uint64_t timeout_ns = kDefaultSetupTimeoutNs,
                            uint8_t max_retries = kDefaultSetupMaxRetries);

    // Produces the initial DATA datagram and arms the retransmit timer.
    size_t start(uint8_t *out, size_t out_cap, uint64_t now_ns);

    // Feeds one received datagram. On a matching HASH-REPLY, emits a FINACK
    // into `out` (returns its length) and marks the handshake matched;
    // re-emits FINACK on a duplicate HASH-REPLY so a lost FINACK recovers.
    // Returns 0 if the datagram produced no reply.
    size_t on_datagram(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                       uint64_t now_ns);

    // Called on the timer. If still awaiting a reply past the timeout,
    // retransmits DATA (returns its length) until the retry cap, after
    // which the handshake fails. Returns 0 if nothing to send.
    size_t on_timeout(uint8_t *out, size_t out_cap, uint64_t now_ns);

    bool is_matched() const { return matched_; }
    bool is_failed() const { return failed_; }
    uint8_t retries() const { return retries_; }
    const SetupPayload &payload() const { return payload_; }

private:
    SetupPayload payload_;
    uint64_t my_hash_ = 0;
    uint64_t timeout_ns_;
    uint8_t  max_retries_;
    uint64_t last_send_ns_ = 0;
    uint8_t  retries_ = 0;
    bool     data_sent_ = false;
    bool     matched_ = false;
    bool     failed_ = false;
};

// The endpoint that accepts the configuration (replies with HASH-REPLY,
// completes on FINACK, and would start its workers on completion).
class SetupResponder {
public:
    explicit SetupResponder(uint64_t timeout_ns = kDefaultSetupTimeoutNs,
                            uint8_t max_retries = kDefaultSetupMaxRetries);

    // Feeds one received datagram. On DATA, records the config, computes
    // its hash, and emits a HASH-REPLY (returns its length); duplicate
    // DATA re-emits the reply. On a matching FINACK, marks complete
    // (returns 0). Returns 0 if the datagram produced no reply.
    size_t on_datagram(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                       uint64_t now_ns);

    // Called on the timer. If a reply was sent but no FINACK has arrived
    // past the timeout, retransmits HASH-REPLY until the retry cap, after
    // which the handshake fails. Returns 0 if nothing to send.
    size_t on_timeout(uint8_t *out, size_t out_cap, uint64_t now_ns);

    bool is_complete() const { return complete_; }
    bool is_failed() const { return failed_; }
    bool has_config() const { return has_config_; }
    const SetupPayload &config() const { return config_; }

private:
    SetupPayload config_;
    uint64_t my_hash_ = 0;
    uint64_t timeout_ns_;
    uint8_t  max_retries_;
    uint64_t last_send_ns_ = 0;
    uint8_t  retries_ = 0;
    bool     has_config_ = false;
    bool     reply_sent_ = false;
    bool     complete_ = false;
    bool     failed_ = false;
};

} // namespace fuse::proto

#endif // FUSE_PROTO_SETUP_HPP
