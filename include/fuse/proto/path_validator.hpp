#ifndef FUSE_PROTO_PATH_VALIDATOR_HPP
#define FUSE_PROTO_PATH_VALIDATOR_HPP

// Path validation (v8, ROADMAP #3). A peer's address is only replaced by a
// new source after that source echoes a random challenge, so a datagram
// with a spoofed or merely different source address cannot redirect where
// a connection sends its traffic (no hijack, no reflection at a third
// party). One candidate at a time; challenges are resent at most every
// 50 ms. Used by the transfer lanes (transfer.cpp) and the SDK (sdk.cpp).

#include <cstdint>

#include "fuse/proto/session_crypto.hpp" // secure_random_u64
#include "fuse/proto/udp.hpp"            // PeerAddr

namespace fuse::proto {

class PathValidator {
public:
    // Traffic arrived from `src`, which is not the current peer. Returns
    // true (and fills *challenge) if a challenge should be sent to it now.
    bool want_probe(const PeerAddr &src, uint64_t now, uint64_t *challenge) {
        if (!(active_ && candidate_ == src)) {
            active_ = true;
            candidate_ = src;
            challenge_ = secure_random_u64();
            sent_ns_ = 0;
        }
        if (sent_ns_ != 0 && now - sent_ns_ < 50'000'000ull) return false;
        sent_ns_ = now;
        *challenge = challenge_;
        return true;
    }
    // A PathResponse arrived; true if it validates the candidate.
    bool validated(const PeerAddr &src, uint64_t value) {
        if (!active_ || !(src == candidate_) || value != challenge_) return false;
        active_ = false;
        return true;
    }

private:
    PeerAddr candidate_{};
    uint64_t challenge_ = 0, sent_ns_ = 0;
    bool active_ = false;
};

} // namespace fuse::proto

#endif // FUSE_PROTO_PATH_VALIDATOR_HPP
