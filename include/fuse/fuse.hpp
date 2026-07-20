#ifndef FUSE_HPP
#define FUSE_HPP

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fuse/fuse.h"

namespace fuse {

class error : public std::runtime_error {
public:
    explicit error(fuse_status status, const std::string &what)
        : std::runtime_error(what), status_(status) {}

    fuse_status status() const noexcept { return status_; }

private:
    fuse_status status_;
};

namespace detail {
inline void check(fuse_status status, const char *what) {
    if (status != FUSE_OK) {
        throw error(status, what);
    }
}
} // namespace detail

inline std::vector<uint8_t> encode_varint(uint64_t value) {
    std::vector<uint8_t> out(8);
    size_t written = fuse_varint_encode(value, out.data(), out.size());
    if (written == 0) {
        throw error(FUSE_ERR_INVALID_ARGUMENT, "encode_varint: value out of range");
    }
    out.resize(written);
    return out;
}

inline std::pair<uint64_t, size_t> decode_varint(const uint8_t *data, size_t len) {
    uint64_t value = 0;
    size_t consumed = fuse_varint_decode(data, len, &value);
    if (consumed == 0) {
        throw error(FUSE_ERR_BUFFER_TOO_SMALL, "decode_varint: truncated input");
    }
    return {value, consumed};
}

/* RAII wrapper around fuse_socket. Move-only, mirroring the
 * non-copyable ownership of the underlying file descriptor. */
class socket {
public:
    socket(const std::string &local_addr, uint16_t local_port) {
        sock_ = fuse_socket_open(local_addr.empty() ? nullptr : local_addr.c_str(), local_port);
        if (!sock_) {
            throw error(FUSE_ERR_SOCKET, "fuse_socket_open failed");
        }
    }

    ~socket() {
        if (sock_) {
            fuse_socket_close(sock_);
        }
    }

    socket(const socket &) = delete;
    socket &operator=(const socket &) = delete;

    socket(socket &&other) noexcept : sock_(std::exchange(other.sock_, nullptr)) {}
    socket &operator=(socket &&other) noexcept {
        if (this != &other) {
            if (sock_) {
                fuse_socket_close(sock_);
            }
            sock_ = std::exchange(other.sock_, nullptr);
        }
        return *this;
    }

    int fd() const noexcept { return fuse_socket_fd(sock_); }

    void set_nonblocking(bool nonblocking) {
        detail::check(fuse_socket_set_nonblocking(sock_, nonblocking ? 1 : 0), "set_nonblocking");
    }

    void send_to(const std::vector<uint8_t> &data, const std::string &dst_addr, uint16_t dst_port) {
        detail::check(
            fuse_socket_send_to(sock_, data.data(), data.size(), dst_addr.c_str(), dst_port),
            "send_to");
    }

    /* Returns the received datagram; on a non-blocking socket with
     * nothing pending, throws with status FUSE_ERR_SOCKET (check
     * errno for EAGAIN/EWOULDBLOCK, matching the C API). */
    std::vector<uint8_t> recv_from(size_t max_len = 65535) {
        std::vector<uint8_t> buf(max_len);
        size_t out_len = 0;
        detail::check(
            fuse_socket_recv_from(sock_, buf.data(), buf.size(), &out_len, nullptr, 0, nullptr),
            "recv_from");
        buf.resize(out_len);
        return buf;
    }

private:
    fuse_socket *sock_ = nullptr;
};

/* RAII wrapper around fuse_connection. See fuse/connection.h for the
 * current (intentionally minimal) state machine this drives. */
class connection {
public:
    connection() : conn_(fuse_connection_new()) {
        if (!conn_) {
            throw error(FUSE_ERR_INVALID_ARGUMENT, "fuse_connection_new failed");
        }
    }

    ~connection() {
        if (conn_) {
            fuse_connection_free(conn_);
        }
    }

    connection(const connection &) = delete;
    connection &operator=(const connection &) = delete;

    connection(connection &&other) noexcept : conn_(std::exchange(other.conn_, nullptr)) {}
    connection &operator=(connection &&other) noexcept {
        if (this != &other) {
            if (conn_) {
                fuse_connection_free(conn_);
            }
            conn_ = std::exchange(other.conn_, nullptr);
        }
        return *this;
    }

    fuse_connection_state state() const noexcept { return fuse_connection_get_state(conn_); }

private:
    fuse_connection *conn_ = nullptr;
};

} // namespace fuse

#endif // FUSE_HPP
