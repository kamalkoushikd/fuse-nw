#ifndef FUSE_TRANSFER_HPP
#define FUSE_TRANSFER_HPP

// Fuse's high-level API: move a buffer or a file reliably between two hosts.
//
// Everything underneath — sharding across lanes, batched syscalls, adaptive
// block sizing, ACK/NACK retransmission, per-stream congestion control and
// optional AEAD encryption — is handled for you. This is the interface most
// applications should use; the headers under fuse/proto/ are the building
// blocks it is made from, for anyone who needs to assemble something else.
//
// Minimal example:
//
//     // receiver
//     fuse::TransferConfig cfg;
//     cfg.bind_address = "0.0.0.0";
//     cfg.base_port = 4433;
//     std::vector<uint8_t> data;
//     fuse::receive_buffer(cfg, &data);
//
//     // sender
//     fuse::TransferConfig cfg;
//     cfg.host = "192.0.2.10";
//     cfg.base_port = 4433;
//     fuse::send_buffer(cfg, data.data(), data.size());
//
// Both calls block until the transfer completes or fails. Start the
// receiver first: it must be bound before the sender's opening message
// arrives.
//
// PORTS. A transfer uses `lanes` consecutive UDP ports starting at
// `base_port` — lane i uses base_port + i. Both ends must agree on
// `base_port` and `lanes`, and the whole range must be open.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fuse {

// A snapshot handed to TransferConfig::on_progress.
struct TransferProgress {
    // Sender: bytes the receiver has acknowledged. Receiver: bytes written.
    uint64_t bytes_done = 0;
    // Total transfer size. Always known on the sender; on the receiver it is
    // 0 until every lane's opening message has arrived.
    uint64_t bytes_total = 0;
    uint64_t retransmits = 0; // sender only
    double seconds = 0.0;     // since the call started
    uint16_t lanes_done = 0;  // lanes that have finished (any outcome)
    uint16_t lanes_total = 0;

    double fraction() const {
        return bytes_total > 0 ? static_cast<double>(bytes_done) / static_cast<double>(bytes_total)
                               : 0.0;
    }
    double mb_per_s() const {
        return seconds > 0.0 ? (static_cast<double>(bytes_done) / (1024.0 * 1024.0)) / seconds
                             : 0.0;
    }
};

struct TransferConfig {
    // Sender: where to send. Receiver: which local address to bind.
    std::string host = "127.0.0.1";
    std::string bind_address = "0.0.0.0";
    uint16_t base_port = 4433;

    // Number of parallel lanes. The file is split into this many shards,
    // each an independent reliable stream. More lanes raise the achievable
    // packet rate, but throughput is NOT monotonic in lane count — beyond a
    // handful, lanes start competing for cores. 4 is a reasonable default;
    // measure before raising it.
    uint16_t lanes = 4;

    // Starting payload bytes per block. The sender adapts upward on a link
    // that stays clean and falls back to this floor on loss, so the default
    // is chosen to be safe under a normal 1500-byte path MTU rather than
    // fast on loopback.
    uint16_t block_size = 1200;

    // Non-empty enables AES-256-GCM. Both ends must supply the same key.
    //
    // This provides confidentiality and integrity with a key that is fresh
    // per session, but NOT forward secrecy: an attacker who records traffic
    // and later obtains this key can decrypt it. Where that matters, use the
    // DTLS path in fuse/proto/dtls.hpp, which negotiates ECDHE-PSK.
    std::string pre_shared_key;

    // Give up if the transfer makes no progress for this long.
    uint32_t timeout_ms = 120000;

    // Sender only: how long to keep trying to reach the receiver before any
    // data flows (like a TCP connect timeout). Kept separate from, and much
    // shorter than, timeout_ms so that a receiver that isn't running — or was
    // stopped before the sender reached it, and so couldn't say so — is
    // reported quickly instead of after the full no-progress timeout.
    uint32_t connect_timeout_ms = 10000;

    // Optional cancellation flag, polled by every lane. Setting it to true
    // (from any thread, or a signal handler — std::atomic<bool> is lock-free)
    // makes the call tell the peer it is aborting and return
    // TransferStatus::Cancelled within a few hundred milliseconds. The peer
    // then returns TransferStatus::PeerAborted instead of waiting out its
    // timeout. Must outlive the call.
    const std::atomic<bool> *cancel = nullptr;

    // Optional progress callback, invoked every progress_interval_ms and
    // once more when the call finishes. It runs on the thread that called
    // send_*/receive_*, never on a lane thread, so it needs no locking of
    // its own; keep it short and don't throw from it.
    std::function<void(const TransferProgress &)> on_progress;
    uint32_t progress_interval_ms = 250;

    // Receiver only: the write pipeline. Each lane's socket thread hands
    // arriving blocks to `writer_threads` writer threads (plus one thread
    // for retransmitted blocks) through lock-free single-producer /
    // single-consumer rings of `ring_slots` packets each; the writers
    // decrypt and write each block straight to its place in the output as
    // it arrives, so receive_file never holds the file in memory.
    // Ring memory is roughly (lanes * writer_threads + lanes) * ring_slots
    // * 16.6 KB — about 51 MB with the defaults and 4 lanes.
    uint16_t writer_threads = 2;
    uint32_t ring_slots = 256;

    // Receiver only (receive_file): how often to save resume progress into
    // the .part file, in milliseconds. It is also saved whenever a transfer
    // ends without completing. 0 = only then. Each save syncs the data
    // written so far, so on a slow disk a longer interval costs less.
    uint32_t resume_checkpoint_ms = 1000;
};

enum class TransferStatus {
    Ok,
    ConfigError,   // nonsensical configuration (no lanes, bad port, ...)
    SocketError,   // bind/connect failed; check the port range and firewall
    Timeout,       // peer went away or the link stalled
    AuthFailed,    // encryption enabled but blocks failed to authenticate
    Incomplete,    // finished without delivering every byte
    Unsupported,   // encryption requested from a build without a crypto backend
    ResourceLimit, // peer's claimed transfer size could not be allocated
    Cancelled,     // TransferConfig::cancel was set on this side
    PeerAborted,   // the other side cancelled or gave up and said so
    IoError,       // receiver: writing the output failed (disk full, permissions, ...)
    VerifyFailed,  // receiver: a resumed file didn't match the sender's digest
};

// Human-readable form, for logs and error messages.
const char *to_string(TransferStatus status);

struct TransferStats {
    uint64_t bytes = 0;
    uint64_t retransmits = 0;
    uint64_t auth_failures = 0;
    double seconds = 0.0;
    uint16_t final_block_size = 0; // what the adaptive sizing settled on
    // Resume: bytes that didn't need sending because the receiver kept them
    // from an earlier, interrupted run (0 for a fresh transfer). `bytes` is
    // what this run actually moved.
    uint64_t resumed_bytes = 0;

    double throughput_mb_per_s() const {
        return seconds > 0.0 ? (static_cast<double>(bytes) / (1024.0 * 1024.0)) / seconds : 0.0;
    }
};

// Sends `len` bytes. Blocks until every byte is acknowledged or the transfer
// fails. `stats` is optional.
TransferStatus send_buffer(const TransferConfig &config, const uint8_t *data, size_t len,
                           TransferStats *stats = nullptr);

// Receives one transfer into `out`, resizing it to the sender's length.
// Blocks until complete or failed.
TransferStatus receive_buffer(const TransferConfig &config, std::vector<uint8_t> *out,
                              TransferStats *stats = nullptr);

// File convenience wrappers. send_file reads the whole file into memory
// first, so it is not suitable for files larger than available RAM.
//
// receive_file writes blocks to disk as they arrive, into "<path>.part",
// and only renames that to <path> once every byte is in and synced. <path>
// itself is never touched by a failed transfer.
//
// RESUME. A transfer made with send_file -> receive_file that stops early
// (Ctrl+C, a dropped connection, a crash) keeps "<path>.part" together with
// a record of which parts of the file it already holds. Running the same
// two commands again — same file, same output path — sends only the missing
// parts, then verifies the whole file against a digest from the sender
// before renaming it into place (VerifyFailed discards the partial copy, so
// the next run starts over). If the file changed in between (different
// name, size or modification time), the transfer simply starts over.
// Delete the .part file to force a fresh start. send_buffer/receive_buffer
// never resume.
TransferStatus send_file(const TransferConfig &config, const std::string &path,
                         TransferStats *stats = nullptr);
TransferStatus receive_file(const TransferConfig &config, const std::string &path,
                            TransferStats *stats = nullptr);

// True if this build can encrypt (i.e. was built with the crypto backend).
// A config with a pre_shared_key on a build where this is false fails with
// TransferStatus::Unsupported rather than sending in the clear.
bool encryption_available();

} // namespace fuse

#endif // FUSE_TRANSFER_HPP
