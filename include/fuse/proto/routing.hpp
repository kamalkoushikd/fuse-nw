#ifndef FUSE_PROTO_ROUTING_HPP
#define FUSE_PROTO_ROUTING_HPP

// Stage 3 static routing: the stream_id -> worker_id table taken directly
// from the SETUP payload (Stage 2). The assignment is fixed for the whole
// session — no rebalancing, no autoscaling — so this is a build-once,
// read-only lookup. worker_id is stored explicitly per stream (never
// recomputed from a formula), matching the SETUP contract, so the two
// endpoints can never disagree about who owns a stream.

#include <cstdint>

#include "fuse/proto/setup.hpp"

namespace fuse::proto {

// A retransmit request the aux thread routes to a worker: "resend this
// block of this stream." It carries no payload — the worker already holds
// the bytes in its own registry.
struct RetransmitRequest {
    uint16_t stream_id = 0;
    uint64_t seq_no = 0;
};

class StreamRouter {
public:
    StreamRouter() = default;
    explicit StreamRouter(const SetupPayload &payload) { build(payload); }

    void build(const SetupPayload &payload) {
        count_ = 0;
        num_workers_ = payload.num_workers;
        for (uint16_t i = 0; i < payload.num_streams && count_ < kMaxStreams; ++i) {
            entries_[count_].stream_id = payload.streams[i].stream_id;
            entries_[count_].worker_id = payload.streams[i].worker_id;
            ++count_;
        }
    }

    uint16_t num_workers() const { return num_workers_; }

    // Returns the owning worker_id for a stream, or -1 if the stream is not
    // in the table. Linear scan over at most kMaxStreams entries; this is
    // aux-thread signalling, not the data hot path.
    int worker_for_stream(uint16_t stream_id) const {
        for (uint16_t i = 0; i < count_; ++i) {
            if (entries_[i].stream_id == stream_id) {
                return entries_[i].worker_id;
            }
        }
        return -1;
    }

private:
    struct Entry {
        uint16_t stream_id = 0;
        uint16_t worker_id = 0;
    };
    Entry entries_[kMaxStreams] = {};
    uint16_t count_ = 0;
    uint16_t num_workers_ = 0;
};

} // namespace fuse::proto

#endif // FUSE_PROTO_ROUTING_HPP
