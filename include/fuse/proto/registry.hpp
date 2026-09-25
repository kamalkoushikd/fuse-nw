#ifndef FUSE_PROTO_REGISTRY_HPP
#define FUSE_PROTO_REGISTRY_HPP

// Stage 1.2 sender-side registry.
//
// One circular array of slots per stream, indexed by seq_no % window_size,
// sized to the stream's negotiated window_size (not the kMaxWindow ceiling)
// so a small-window stream doesn't pay for kMaxWindow's worst case. Every
// byte a slot needs is embedded in the slot itself (the payload is a fixed
// kMaxPayloadSize array, not a pointer), so beyond the one allocation at
// construction there is no further heap activity on the send/retransmit
// path. A slot is overwritten only when the window advances past its
// seq_no; a NACK for a seq_no still resident in its slot can always be
// served from memory.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "fuse/proto/wire.hpp"

namespace fuse::proto {

struct RegistrySlot {
    uint64_t seq_no       = 0;
    uint64_t send_time_ns = 0;
    // Byte offset of this block within its stream. Stored rather than
    // derived as seq_no * block_size, since block size adapts mid-stream
    // (wire.hpp v2) — that derivation would misplace a retransmit of any
    // block sent before an adaptation.
    uint64_t offset       = 0;
    uint16_t payload_len  = 0;
    bool     valid        = false;
    uint8_t  payload[kMaxPayloadSize] = {};
};

class SenderRegistry {
public:
    // window_size is clamped to [1, kMaxWindow]; the storage is always
    // kMaxWindow slots, but only the first window_size are used, matching
    // the per-stream window negotiated at SETUP (Stage 2).
    explicit SenderRegistry(uint16_t stream_id, uint8_t window_size);

    uint16_t stream_id() const { return stream_id_; }
    uint8_t  window_size() const { return window_size_; }

    // Records a to-be-sent block, overwriting whatever slot its seq_no
    // maps to, and marks the slot valid. Returns false if payload_len
    // exceeds kMaxPayloadSize. Caller sends the datagram separately (the
    // registry is storage, not I/O).
    bool store(uint64_t seq_no, const uint8_t *payload, uint16_t payload_len,
               uint64_t send_time_ns, uint64_t offset);

    // Looks up a slot for retransmission. Returns a pointer to the slot
    // iff it is valid and still holds exactly this seq_no (i.e. has not
    // been overwritten by a later store to the same index); otherwise
    // nullptr — an "unrecoverable" NACK the caller should count.
    const RegistrySlot *lookup(uint64_t seq_no) const;

    // Marks the slot for seq_no invalid (evictable) once an ACK confirms
    // delivery. A no-op if the slot no longer holds this seq_no.
    void confirm(uint64_t seq_no);

    // Number of currently-valid (unacked, resident) slots — a metric, not
    // used on the hot path.
    uint8_t valid_count() const;

private:
    size_t index_of(uint64_t seq_no) const {
        return static_cast<size_t>(seq_no % window_size_);
    }

    uint16_t stream_id_;
    uint8_t  window_size_;
    std::vector<RegistrySlot> slots_;
};

} // namespace fuse::proto

#endif // FUSE_PROTO_REGISTRY_HPP
