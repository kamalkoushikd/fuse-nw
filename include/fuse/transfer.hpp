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

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fuse {

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
};

enum class TransferStatus {
    Ok,
    ConfigError,   // nonsensical configuration (no lanes, bad port, ...)
    SocketError,   // bind/connect failed; check the port range and firewall
    Timeout,       // peer went away or the link stalled
    AuthFailed,    // encryption enabled but blocks failed to authenticate
    Incomplete,    // finished without delivering every byte
    Unsupported,   // encryption requested from a build without a crypto backend
};

// Human-readable form, for logs and error messages.
const char *to_string(TransferStatus status);

struct TransferStats {
    uint64_t bytes = 0;
    uint64_t retransmits = 0;
    uint64_t auth_failures = 0;
    double seconds = 0.0;
    uint16_t final_block_size = 0; // what the adaptive sizing settled on

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
