#ifndef FUSE_PROTO_BLOCK_HPP
#define FUSE_PROTO_BLOCK_HPP

// Stage 1.1 block header and DATA-datagram framing.
//
// A "block" is one independently-addressed, independently-retransmittable
// datagram. Its 13-byte header identifies the stream and the per-stream
// monotonic sequence number the sender assigned it. The full DATA
// datagram on the wire is:
//
//   [version:1][msg_type=Data:1]                 <- outer header (wire.hpp)
//   [stream_id:2][seq_no:8][flags:1][payload_len:2]  <- block header (13B)
//   [send_time_ns:8]                             <- for the ACK RTT echo
//   [payload: payload_len bytes]
//
// send_time_ns is not part of the formal block header, but is transmitted
// alongside it so the receiver can echo it in an ACK; this is what lets
// Stage 5 measure RTT in real elapsed time without a later wire break.

#include <cstddef>
#include <cstdint>

#include "fuse/proto/wire.hpp"

namespace fuse::proto {

struct BlockHeader {
    uint16_t stream_id   = 0;
    uint64_t seq_no      = 0;
    uint8_t  flags       = 0;
    uint16_t payload_len = 0;
    // Byte position of this block's payload within its stream. Carried
    // explicitly rather than derived as seq_no * block_size, so a sender may
    // change block size mid-stream (adapting to link conditions) without the
    // receiver misplacing every subsequent block.
    uint64_t offset      = 0;
};

// Encodes a complete DATA datagram (outer header + block header +
// send_time + payload) into `out`. Returns the total datagram length, or
// 0 if `out_cap` is too small or payload_len exceeds kMaxPayloadSize.
size_t encode_data_datagram(const BlockHeader &hdr,
                            uint64_t send_time_ns,
                            const uint8_t *payload,
                            uint8_t *out, size_t out_cap);

// Decodes a DATA datagram from `in` (`in_len` bytes). On success, fills
// *hdr and *send_time_ns, sets *payload to point into `in`, and returns
// true. Returns false on a version mismatch, a wrong/unknown msg_type, a
// truncated datagram, or a payload_len inconsistent with in_len.
bool decode_data_datagram(const uint8_t *in, size_t in_len,
                          BlockHeader *hdr, uint64_t *send_time_ns,
                          const uint8_t **payload);

// Reads just the outer header's msg_type, so a receive loop can dispatch
// a datagram to the right decoder. Returns false on version mismatch or
// a datagram too short to hold an outer header.
bool peek_msg_type(const uint8_t *in, size_t in_len, MsgType *type);

} // namespace fuse::proto

#endif // FUSE_PROTO_BLOCK_HPP
