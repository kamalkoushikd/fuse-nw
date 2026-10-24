#include "fuse/proto/registry.hpp"

#include <algorithm>
#include <cstring>

namespace fuse::proto {

SenderRegistry::SenderRegistry(uint16_t stream_id, uint16_t window_size)
    : stream_id_(stream_id) {
    if (window_size < 1) {
        window_size = 1;
    } else if (window_size > kMaxWindow) {
        window_size = kMaxWindow;
    }
    window_size_ = window_size;
    slots_.resize(window_size_);
}

bool SenderRegistry::store(uint64_t seq_no, const uint8_t *payload, uint16_t payload_len,
                           uint64_t send_time_ns, uint64_t offset, uint8_t flags) {
    if (payload_len > kMaxPayloadSize) {
        return false;
    }
    RegistrySlot &slot = slots_[index_of(seq_no)];
    slot.seq_no = seq_no;
    slot.send_time_ns = send_time_ns;
    slot.offset = offset;
    slot.payload_len = payload_len;
    slot.flags = flags;
    slot.valid = true;
    if (payload_len > 0 && payload != nullptr) {
        std::memcpy(slot.payload, payload, payload_len);
    }
    return true;
}

const RegistrySlot *SenderRegistry::lookup(uint64_t seq_no) const {
    const RegistrySlot &slot = slots_[index_of(seq_no)];
    if (slot.valid && slot.seq_no == seq_no) {
        return &slot;
    }
    return nullptr;
}

void SenderRegistry::confirm(uint64_t seq_no) {
    RegistrySlot &slot = slots_[index_of(seq_no)];
    if (slot.valid && slot.seq_no == seq_no) {
        slot.valid = false;
    }
}

uint64_t apply_ack(SenderRegistry &reg, const Ack &ack, uint64_t *base, uint64_t next_seq,
                   uint64_t *received_end) {
    uint64_t bytes = 0;
    const auto take = [&](uint64_t seq) {
        if (const RegistrySlot *sl = reg.lookup(seq)) {
            bytes += sl->payload_len; // count before confirm() frees the slot
            reg.confirm(seq);
        }
    };
    if (ack.base_seq_no > *base) {
        const uint64_t new_base = std::min(ack.base_seq_no, next_seq);
        for (uint64_t s = *base; s < new_base; ++s) take(s);
        *base = new_base;
    }
    // Selective part: bit i means ack.base_seq_no + i arrived.
    if (ack.base_seq_no < next_seq) {
        for (size_t w = 0; w < kMaskWords; ++w) {
            uint64_t bits = ack.received_bitmask[w];
            while (bits != 0) {
                const unsigned b = static_cast<unsigned>(__builtin_ctzll(bits));
                bits &= bits - 1;
                const uint64_t seq = ack.base_seq_no + 64 * w + b;
                if (seq >= next_seq) break;
                if (seq >= *base) take(seq);
                if (received_end && seq + 1 > *received_end) *received_end = seq + 1;
            }
        }
    }
    if (received_end && *base > *received_end) *received_end = *base;
    return bytes;
}

uint16_t SenderRegistry::valid_count() const {
    uint16_t n = 0;
    for (uint16_t i = 0; i < window_size_; ++i) {
        if (slots_[i].valid) {
            ++n;
        }
    }
    return n;
}

} // namespace fuse::proto
