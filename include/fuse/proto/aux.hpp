#ifndef FUSE_PROTO_AUX_HPP
#define FUSE_PROTO_AUX_HPP

// Stage 1.5 aux channel: the signalling messages that ride alongside the
// data plane, distinguished from DATA by their outer-header msg_type.
// None of these carry payload bytes; they only carry control signals.
//
//   Heartbeat : [outer][stream_id:2][highest_seq_no:8]
//   Ack       : [outer][stream_id:2][base_seq_no:8][received_bitmask:8][echoed_send_time:8]
//   Nack      : [outer][stream_id:2][count:2][missing_seq_no:8]*count
//
// The ACK's echoed_send_time is the send_time_ns of the most recent DATA
// block the receiver has accepted, copied straight back so the sender can
// compute RTT (Stage 5) against its own clock.

#include <cstddef>
#include <cstdint>

#include "fuse/proto/wire.hpp"

namespace fuse::proto {

struct Heartbeat {
    uint16_t stream_id      = 0;
    uint64_t highest_seq_no = 0;
};

// Sent at the start of a bulk stream (Stage 4.2), not in session-wide SETUP:
// it tells the receiver how long this particular stream is, so a gap at the
// tail can be distinguished from "more is still coming". Retransmitted until
// the receiver's first ACK confirms it arrived.
struct StreamStart {
    uint16_t stream_id    = 0;
    uint64_t total_blocks = 0;
    uint16_t block_size   = 0;
    uint64_t total_bytes  = 0;
};

struct Ack {
    uint16_t stream_id        = 0;
    uint64_t base_seq_no      = 0;
    uint64_t received_bitmask = 0;
    uint64_t echoed_send_time = 0;
};

// A NACK names up to kMaxWindow missing sequence numbers for one stream —
// bounded by the window, since nothing outside the window is trackable.
struct Nack {
    uint16_t stream_id = 0;
    uint16_t count     = 0;
    uint64_t missing[kMaxWindow] = {};
};

// Encode helpers return the datagram length written, or 0 if out_cap is
// too small. Decode helpers return true on success, false on version
// mismatch, wrong msg_type, or truncation.

size_t encode_heartbeat(const Heartbeat &hb, uint8_t *out, size_t out_cap);
bool   decode_heartbeat(const uint8_t *in, size_t in_len, Heartbeat *hb);

size_t encode_stream_start(const StreamStart &ss, uint8_t *out, size_t out_cap);
bool   decode_stream_start(const uint8_t *in, size_t in_len, StreamStart *ss);

size_t encode_ack(const Ack &ack, uint8_t *out, size_t out_cap);
bool   decode_ack(const uint8_t *in, size_t in_len, Ack *ack);

size_t encode_nack(const Nack &nack, uint8_t *out, size_t out_cap);
bool   decode_nack(const uint8_t *in, size_t in_len, Nack *nack);

// Largest aux datagram: a full-window NACK. Used to size receive buffers.
inline constexpr size_t kMaxAuxDatagramSize =
    kOuterHeaderSize + 2 /*stream_id*/ + 2 /*count*/ + kMaxWindow * 8;

} // namespace fuse::proto

#endif // FUSE_PROTO_AUX_HPP
