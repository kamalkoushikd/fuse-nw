#ifndef FUSE_MUX_TRANSFER_HPP
#define FUSE_MUX_TRANSFER_HPP

// Multiplexed transfer: several logical streams share ONE UDP socket and a
// dynamically-scaled pool of worker threads, instead of transfer.hpp's one
// thread (and one socket) per lane. Where transfer.hpp's lane count is fixed
// for the whole call, here the number of active worker threads adapts to
// load while the transfer is running (fuse::proto::WorkerOrchestrator), and
// streams are serviced by whichever workers are alive rather than each
// stream permanently owning a thread.
//
// Use this when you want more logical streams than you want OS threads —
// send_buffer()/receive_buffer() still make sense when lane count and
// thread count should just be the same number.
//
// Not (yet) supported here, unlike transfer.hpp: encryption, NAT-migration
// re-anchoring of the peer address, adaptive block-size growth, and GSO
// batching. See include/fuse/proto/ for the pieces this is built from.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fuse/transfer.hpp"

namespace fuse {

struct MuxTransferConfig {
    // Sender: where to send. Receiver: which local address to bind.
    std::string host = "127.0.0.1";
    std::string bind_address = "0.0.0.0";
    uint16_t port = 4433;

    // Logical streams multiplexed over the one socket above. Unlike
    // TransferConfig::lanes, this is not the thread count — worker_count
    // below controls that, independently and dynamically.
    uint16_t num_streams = 4;

    uint16_t block_size = 1200;
    uint16_t window_size = 64; // per-stream in-flight cap, <= 1024

    // fuse::proto::WorkerOrchestrator tuning — field names and defaults
    // mirror OrchestratorConfig directly (see fuse/proto/orchestrator.hpp).
    uint16_t min_workers = 1;
    uint16_t max_workers = 8;
    double target_utilization = 0.70;
    double scale_in_utilization = 0.30;
    uint64_t stabilization_ns = 200'000'000;

    // Give up if the transfer makes no progress for this long.
    uint32_t timeout_ms = 120000;
};

// Orchestrator-specific observability that TransferStats has no field for
// (it's shared with transfer.hpp's fixed-thread-count model, which has
// nothing to report here). Optional — most callers only need TransferStatus.
struct MuxTransferStats {
    uint16_t peak_workers = 0; // highest concurrent worker count observed
    uint64_t scale_outs = 0;
    uint64_t scale_ins = 0;
};

// Sends `len` bytes across `config.num_streams` multiplexed streams. Blocks
// until every stream is acknowledged or the transfer fails. `stats` and
// `mux_stats` are optional. Reuses fuse::TransferStatus/TransferStats from
// transfer.hpp.
TransferStatus send_multiplexed(const MuxTransferConfig &config, const uint8_t *data, size_t len,
                                TransferStats *stats = nullptr,
                                MuxTransferStats *mux_stats = nullptr);

// Receives one multiplexed transfer into `out`. Blocks until complete or
// failed. `*out` is only populated when the result is Ok, matching
// receive_buffer()'s contract.
TransferStatus receive_multiplexed(const MuxTransferConfig &config, std::vector<uint8_t> *out,
                                   TransferStats *stats = nullptr,
                                   MuxTransferStats *mux_stats = nullptr);

} // namespace fuse

#endif // FUSE_MUX_TRANSFER_HPP
