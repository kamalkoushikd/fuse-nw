#include "fuse/proto/registry.hpp"

#include <cstring>

namespace fuse::proto {

SenderRegistry::SenderRegistry(uint16_t stream_id, uint8_t window_size)
    : stream_id_(stream_id) {
    if (window_size < 1) {
        window_size = 1;
    } else if (window_size > kMaxWindow) {
        window_size = kMaxWindow;
    }
    window_size_ = window_size;
}

bool SenderRegistry::store(uint64_t seq_no, const uint8_t *payload,
                           uint16_t payload_len, uint64_t send_time_ns) {
    if (payload_len > kMaxPayloadSize) {
        return false;
    }
    RegistrySlot &slot = slots_[index_of(seq_no)];
    slot.seq_no = seq_no;
    slot.send_time_ns = send_time_ns;
    slot.payload_len = payload_len;
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

uint8_t SenderRegistry::valid_count() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < window_size_; ++i) {
        if (slots_[i].valid) {
            ++n;
        }
    }
    return n;
}

} // namespace fuse::proto
