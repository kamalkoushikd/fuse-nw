#include "fuse/proto/block.hpp"

#include <cstring>

namespace fuse::proto {

bool peek_msg_type(const uint8_t *in, size_t in_len, MsgType *type) {
    if (in_len < kOuterHeaderSize) {
        return false;
    }
    if (in[0] != kProtocolVersion) {
        return false;
    }
    *type = static_cast<MsgType>(in[1]);
    return true;
}

size_t encode_data_datagram(const BlockHeader &hdr,
                            uint64_t send_time_ns,
                            const uint8_t *payload,
                            uint8_t *out, size_t out_cap) {
    if (hdr.payload_len > kMaxPayloadSize) {
        return 0;
    }
    const size_t total = kDataPrefixSize + hdr.payload_len;
    if (out_cap < total) {
        return 0;
    }

    size_t off = 0;
    off += put_u8(out + off, kProtocolVersion);
    off += put_u8(out + off, static_cast<uint8_t>(MsgType::Data));
    off += put_u16(out + off, hdr.stream_id);
    off += put_u64(out + off, hdr.seq_no);
    off += put_u8(out + off, hdr.flags);
    off += put_u16(out + off, hdr.payload_len);
    off += put_u64(out + off, hdr.offset);
    off += put_u64(out + off, send_time_ns);

    if (hdr.payload_len > 0 && payload != nullptr) {
        std::memcpy(out + off, payload, hdr.payload_len);
    }
    off += hdr.payload_len;

    return off;
}

bool decode_data_datagram(const uint8_t *in, size_t in_len,
                          BlockHeader *hdr, uint64_t *send_time_ns,
                          const uint8_t **payload) {
    if (in_len < kDataPrefixSize) {
        return false;
    }

    size_t off = 0;
    uint8_t version = 0;
    uint8_t msg_type = 0;
    off += get_u8(in + off, &version);
    off += get_u8(in + off, &msg_type);
    if (version != kProtocolVersion || static_cast<MsgType>(msg_type) != MsgType::Data) {
        return false;
    }

    off += get_u16(in + off, &hdr->stream_id);
    off += get_u64(in + off, &hdr->seq_no);
    off += get_u8(in + off, &hdr->flags);
    off += get_u16(in + off, &hdr->payload_len);
    off += get_u64(in + off, &hdr->offset);
    off += get_u64(in + off, send_time_ns);

    if (hdr->payload_len > kMaxPayloadSize) {
        return false;
    }
    if (in_len != kDataPrefixSize + hdr->payload_len) {
        return false;
    }

    *payload = (hdr->payload_len > 0) ? (in + off) : nullptr;
    return true;
}

} // namespace fuse::proto
