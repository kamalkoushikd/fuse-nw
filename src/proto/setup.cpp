#include "fuse/proto/setup.hpp"

#include "fuse/proto/block.hpp" // peek_msg_type
#include "fuse/proto/hash.hpp"

namespace fuse::proto {

size_t serialize_setup_payload(const SetupPayload &p, uint8_t *out, size_t out_cap) {
    if (p.num_streams > kMaxStreams) {
        return 0;
    }
    const size_t total = 1 + 2 + 2 + static_cast<size_t>(p.num_streams) * 8;
    if (out_cap < total) {
        return 0;
    }

    size_t off = 0;
    off += put_u8(out + off, p.protocol_version);
    off += put_u16(out + off, p.num_workers);
    off += put_u16(out + off, p.num_streams);
    for (uint16_t i = 0; i < p.num_streams; ++i) {
        const StreamConfig &s = p.streams[i];
        off += put_u16(out + off, s.stream_id);
        off += put_u16(out + off, s.worker_id);
        off += put_u8(out + off, s.stream_flags);
        off += put_u16(out + off, s.block_size);
        off += put_u8(out + off, s.window_size);
    }
    return off;
}

size_t encode_setup_data(const SetupPayload &p, uint8_t *out, size_t out_cap) {
    if (out_cap < kOuterHeaderSize) {
        return 0;
    }
    size_t off = 0;
    off += put_u8(out + off, kProtocolVersion);
    off += put_u8(out + off, static_cast<uint8_t>(MsgType::SetupData));

    size_t body = serialize_setup_payload(p, out + off, out_cap - off);
    if (body == 0 && p.num_streams != 0) {
        return 0;
    }
    // body can legitimately be the fixed prefix (5 bytes) with zero streams;
    // serialize returns >0 in that case, so body==0 only means overflow.
    if (body == 0) {
        return 0;
    }
    return off + body;
}

bool decode_setup_data(const uint8_t *in, size_t in_len, SetupPayload *p,
                       const uint8_t **raw_body, size_t *raw_body_len) {
    if (in_len < kOuterHeaderSize + 1 + 2 + 2) {
        return false;
    }
    if (in[0] != kProtocolVersion || static_cast<MsgType>(in[1]) != MsgType::SetupData) {
        return false;
    }

    const uint8_t *body = in + kOuterHeaderSize;
    size_t body_len = in_len - kOuterHeaderSize;

    size_t off = 0;
    off += get_u8(body + off, &p->protocol_version);
    off += get_u16(body + off, &p->num_workers);
    off += get_u16(body + off, &p->num_streams);

    if (p->num_streams > kMaxStreams) {
        return false;
    }
    const size_t expected = 1 + 2 + 2 + static_cast<size_t>(p->num_streams) * 8;
    if (body_len != expected) {
        return false;
    }

    for (uint16_t i = 0; i < p->num_streams; ++i) {
        StreamConfig &s = p->streams[i];
        off += get_u16(body + off, &s.stream_id);
        off += get_u16(body + off, &s.worker_id);
        off += get_u8(body + off, &s.stream_flags);
        off += get_u16(body + off, &s.block_size);
        off += get_u8(body + off, &s.window_size);
    }

    if (raw_body) *raw_body = body;
    if (raw_body_len) *raw_body_len = body_len;
    return true;
}

size_t encode_setup_hash(MsgType type, uint64_t hash, uint8_t *out, size_t out_cap) {
    if (type != MsgType::SetupHashReply && type != MsgType::SetupFinAck) {
        return 0;
    }
    const size_t total = kOuterHeaderSize + 8;
    if (out_cap < total) {
        return 0;
    }
    size_t off = 0;
    off += put_u8(out + off, kProtocolVersion);
    off += put_u8(out + off, static_cast<uint8_t>(type));
    off += put_u64(out + off, hash);
    return off;
}

bool decode_setup_hash(MsgType expected, const uint8_t *in, size_t in_len, uint64_t *hash) {
    if (in_len != kOuterHeaderSize + 8) {
        return false;
    }
    if (in[0] != kProtocolVersion || static_cast<MsgType>(in[1]) != expected) {
        return false;
    }
    get_u64(in + kOuterHeaderSize, hash);
    return true;
}

// --- SetupInitiator ------------------------------------------------------

SetupInitiator::SetupInitiator(const SetupPayload &payload, uint64_t timeout_ns,
                               uint8_t max_retries)
    : payload_(payload), timeout_ns_(timeout_ns), max_retries_(max_retries) {
    uint8_t body[kMaxSetupDatagramSize];
    size_t n = serialize_setup_payload(payload_, body, sizeof(body));
    my_hash_ = hash64(body, n);
}

size_t SetupInitiator::start(uint8_t *out, size_t out_cap, uint64_t now_ns) {
    size_t n = encode_setup_data(payload_, out, out_cap);
    if (n == 0) {
        failed_ = true;
        return 0;
    }
    data_sent_ = true;
    last_send_ns_ = now_ns;
    return n;
}

size_t SetupInitiator::on_datagram(const uint8_t *in, size_t in_len, uint8_t *out,
                                   size_t out_cap, uint64_t now_ns) {
    MsgType type;
    if (!peek_msg_type(in, in_len, &type) || type != MsgType::SetupHashReply) {
        return 0;
    }
    uint64_t their_hash = 0;
    if (!decode_setup_hash(MsgType::SetupHashReply, in, in_len, &their_hash)) {
        return 0;
    }
    if (their_hash != my_hash_) {
        // Corrupted config or reply: do not confirm. Silence lets the
        // retransmit timer resend DATA and try again.
        return 0;
    }

    // Match (possibly a duplicate HASH-REPLY after a lost FINACK): (re)emit
    // FINACK carrying our hash so the responder can complete.
    matched_ = true;
    last_send_ns_ = now_ns;
    return encode_setup_hash(MsgType::SetupFinAck, my_hash_, out, out_cap);
}

size_t SetupInitiator::on_timeout(uint8_t *out, size_t out_cap, uint64_t now_ns) {
    if (matched_ || failed_ || !data_sent_) {
        return 0; // once matched, recovery is driven by the responder
    }
    if (now_ns - last_send_ns_ < timeout_ns_) {
        return 0;
    }
    if (retries_ >= max_retries_) {
        failed_ = true;
        return 0;
    }
    ++retries_;
    last_send_ns_ = now_ns;
    return encode_setup_data(payload_, out, out_cap);
}

// --- SetupResponder ------------------------------------------------------

SetupResponder::SetupResponder(uint64_t timeout_ns, uint8_t max_retries)
    : timeout_ns_(timeout_ns), max_retries_(max_retries) {}

size_t SetupResponder::on_datagram(const uint8_t *in, size_t in_len, uint8_t *out,
                                   size_t out_cap, uint64_t now_ns) {
    MsgType type;
    if (!peek_msg_type(in, in_len, &type)) {
        return 0;
    }

    if (type == MsgType::SetupData) {
        const uint8_t *body = nullptr;
        size_t body_len = 0;
        SetupPayload decoded;
        if (!decode_setup_data(in, in_len, &decoded, &body, &body_len)) {
            return 0; // malformed; ignore and let the initiator resend
        }
        config_ = decoded;
        has_config_ = true;
        my_hash_ = hash64(body, body_len); // hash exactly what arrived
        reply_sent_ = true;
        last_send_ns_ = now_ns;
        return encode_setup_hash(MsgType::SetupHashReply, my_hash_, out, out_cap);
    }

    if (type == MsgType::SetupFinAck) {
        uint64_t their_hash = 0;
        if (!decode_setup_hash(MsgType::SetupFinAck, in, in_len, &their_hash)) {
            return 0;
        }
        if (reply_sent_ && their_hash == my_hash_) {
            complete_ = true; // this is where workers would be started
        }
        return 0;
    }

    return 0;
}

size_t SetupResponder::on_timeout(uint8_t *out, size_t out_cap, uint64_t now_ns) {
    if (complete_ || failed_ || !reply_sent_) {
        return 0;
    }
    if (now_ns - last_send_ns_ < timeout_ns_) {
        return 0;
    }
    if (retries_ >= max_retries_) {
        failed_ = true;
        return 0;
    }
    ++retries_;
    last_send_ns_ = now_ns;
    // Resend HASH-REPLY only — the responder's copy of the data is fine; it
    // just needs to prompt the initiator for a FINACK again.
    return encode_setup_hash(MsgType::SetupHashReply, my_hash_, out, out_cap);
}

} // namespace fuse::proto
