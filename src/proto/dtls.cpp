#include "fuse/proto/dtls.hpp"

#include <cstring>

#if FUSE_PROTO_WITH_DTLS
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#endif

namespace fuse::proto {

bool dtls_available() {
    return FUSE_PROTO_WITH_DTLS != 0;
}

DtlsSession::DtlsSession() = default;

#if !FUSE_PROTO_WITH_DTLS

// --- Build without wolfSSL/DTLS -----------------------------------------
//
// Encryption is unavailable. Requiring it must fail closed; not requiring it
// leaves the layer a transparent pass-through over the plain socket.

DtlsSession::~DtlsSession() = default;

DtlsStatus DtlsSession::configure(const DtlsConfig &config, UdpSocket *socket,
                                  const PeerAddr &peer) {
    config_ = config;
    socket_ = socket;
    peer_ = peer;
    if (config.encryption_required) {
        return DtlsStatus::Unsupported; // fail closed, never fall back to plaintext
    }
    encryption_active_ = false;
    return DtlsStatus::Disabled;
}

DtlsStatus DtlsSession::handshake() {
    if (config_.encryption_required) {
        return DtlsStatus::Unsupported;
    }
    established_ = true;
    return DtlsStatus::Disabled;
}

#else // FUSE_PROTO_WITH_DTLS

// --- wolfSSL-backed DTLS 1.2 with pre-shared keys ------------------------

namespace {

// Both sides share one identity/key pair for the link. wolfSSL hands the
// callback a fixed-size buffer; we copy in the configured key. The session's
// config travels via wolfSSL ex-data so callbacks stay per-session.
unsigned int psk_client_cb(WOLFSSL *ssl, const char *, char *identity,
                           unsigned int id_max_len, unsigned char *key,
                           unsigned int key_max_len) {
    auto *cfg = static_cast<DtlsConfig *>(wolfSSL_get_ex_data(ssl, 0));
    if (cfg == nullptr) return 0;
    if (cfg->psk_identity.size() + 1 > id_max_len) return 0;
    std::memcpy(identity, cfg->psk_identity.c_str(), cfg->psk_identity.size() + 1);
    if (cfg->psk_key.size() > key_max_len) return 0;
    std::memcpy(key, cfg->psk_key.data(), cfg->psk_key.size());
    return static_cast<unsigned int>(cfg->psk_key.size());
}

unsigned int psk_server_cb(WOLFSSL *ssl, const char *identity, unsigned char *key,
                           unsigned int key_max_len) {
    auto *cfg = static_cast<DtlsConfig *>(wolfSSL_get_ex_data(ssl, 0));
    if (cfg == nullptr) return 0;
    // A wrong identity is rejected here; a wrong key is rejected by the
    // handshake's own verification.
    if (cfg->psk_identity != identity) return 0;
    if (cfg->psk_key.size() > key_max_len) return 0;
    std::memcpy(key, cfg->psk_key.data(), cfg->psk_key.size());
    return static_cast<unsigned int>(cfg->psk_key.size());
}

} // namespace

DtlsSession::~DtlsSession() {
    if (ssl_ != nullptr) {
        wolfSSL_free(static_cast<WOLFSSL *>(ssl_));
        ssl_ = nullptr;
    }
    if (ctx_ != nullptr) {
        wolfSSL_CTX_free(static_cast<WOLFSSL_CTX *>(ctx_));
        ctx_ = nullptr;
    }
}

int DtlsSession::wire_send(const uint8_t *buf, size_t len) {
    if (socket_ == nullptr || len == 0) {
        return WOLFSSL_CBIO_ERR_GENERAL;
    }
    if (!socket_->send_to(buf, len, peer_)) {
        return WOLFSSL_CBIO_ERR_GENERAL;
    }
    // Record what actually went on the wire so callers can confirm it is
    // ciphertext, not plaintext.
    wire_bytes_sent_ += static_cast<uint64_t>(len);
    size_t keep = len > sizeof(last_wire_.data) ? sizeof(last_wire_.data) : len;
    std::memcpy(last_wire_.data, buf, keep);
    last_wire_.len = keep;
    return static_cast<int>(len);
}

int DtlsSession::wire_recv(uint8_t *buf, size_t cap) {
    if (socket_ == nullptr || cap == 0) {
        return WOLFSSL_CBIO_ERR_GENERAL;
    }
    size_t got = 0;
    PeerAddr from;
    if (!socket_->recv_from(buf, cap, &got, &from)) {
        // A receive timeout is how a blocking DTLS retransmit cycle learns
        // to resend; report it as such rather than a hard error.
        return WOLFSSL_CBIO_ERR_TIMEOUT;
    }
    // A server learns its peer's address from the first datagram received.
    if (config_.role == DtlsRole::Server) {
        peer_ = from;
    }
    return static_cast<int>(got);
}

namespace {

// wolfSSL custom I/O trampolines. Their signatures must match
// CallbackIOSend/CallbackIORecv exactly; each forwards to the DtlsSession
// carried in the I/O context.
int dtls_io_send(WOLFSSL *, char *buf, int sz, void *ctx) {
    auto *self = static_cast<DtlsSession *>(ctx);
    if (self == nullptr || sz <= 0) return WOLFSSL_CBIO_ERR_GENERAL;
    return self->wire_send(reinterpret_cast<const uint8_t *>(buf), static_cast<size_t>(sz));
}

int dtls_io_recv(WOLFSSL *, char *buf, int sz, void *ctx) {
    auto *self = static_cast<DtlsSession *>(ctx);
    if (self == nullptr || sz <= 0) return WOLFSSL_CBIO_ERR_GENERAL;
    return self->wire_recv(reinterpret_cast<uint8_t *>(buf), static_cast<size_t>(sz));
}

} // namespace

DtlsStatus DtlsSession::configure(const DtlsConfig &config, UdpSocket *socket,
                                  const PeerAddr &peer) {
    config_ = config;
    socket_ = socket;
    peer_ = peer;

    if (!config_.encryption_required) {
        encryption_active_ = false;
        return DtlsStatus::Disabled; // transparent pass-through
    }
    if (socket_ == nullptr || config_.psk_key.empty()) {
        return DtlsStatus::HandshakeFailed;
    }

    wolfSSL_Init();
    WOLFSSL_METHOD *method = (config_.role == DtlsRole::Server)
                                 ? wolfDTLSv1_2_server_method()
                                 : wolfDTLSv1_2_client_method();
    if (method == nullptr) {
        return DtlsStatus::Unsupported;
    }
    WOLFSSL_CTX *ctx = wolfSSL_CTX_new(method);
    if (ctx == nullptr) {
        return DtlsStatus::Unsupported;
    }
    ctx_ = ctx;

    if (config_.role == DtlsRole::Server) {
        wolfSSL_CTX_set_psk_server_callback(ctx, psk_server_cb);
        wolfSSL_CTX_use_psk_identity_hint(ctx, config_.psk_identity.c_str());
    } else {
        wolfSSL_CTX_set_psk_client_callback(ctx, psk_client_cb);
    }
    // PSK-only suites: no certificates are involved on either side. The
    // ephemeral-key variants are listed first deliberately — an ECDHE/DHE
    // PSK exchange gives forward secrecy, so recording the traffic and
    // later learning the pre-shared key does not retroactively decrypt it.
    // Static PSK (no ephemeral key) is the last resort and is not even
    // compiled into wolfSSL by default for exactly that reason.
    if (wolfSSL_CTX_set_cipher_list(ctx,
                                    "ECDHE-PSK-AES128-GCM-SHA256:"
                                    "DHE-PSK-AES128-GCM-SHA256:"
                                    "ECDHE-PSK-CHACHA20-POLY1305:"
                                    "PSK-CHACHA20-POLY1305") != WOLFSSL_SUCCESS) {
        return DtlsStatus::Unsupported;
    }

    wolfSSL_CTX_SetIOSend(ctx, dtls_io_send);
    wolfSSL_CTX_SetIORecv(ctx, dtls_io_recv);

    WOLFSSL *ssl = wolfSSL_new(ctx);
    if (ssl == nullptr) {
        return DtlsStatus::Unsupported;
    }
    ssl_ = ssl;

    wolfSSL_set_ex_data(ssl, 0, &config_);
    wolfSSL_SetIOWriteCtx(ssl, this);
    wolfSSL_SetIOReadCtx(ssl, this);

    // Bound the DTLS retransmit backoff so a doomed handshake (mismatched
    // PSK, unreachable peer) reports failure promptly instead of retrying
    // for the better part of a minute.
    if (config_.handshake_timeout_init_sec > 0) {
        wolfSSL_dtls_set_timeout_init(ssl, config_.handshake_timeout_init_sec);
    }
    if (config_.handshake_timeout_max_sec > 0) {
        wolfSSL_dtls_set_timeout_max(ssl, config_.handshake_timeout_max_sec);
    }

    encryption_active_ = true;
    return DtlsStatus::Ok;
}

DtlsStatus DtlsSession::handshake() {
    if (!config_.encryption_required) {
        established_ = true;
        return DtlsStatus::Disabled;
    }
    if (ssl_ == nullptr) {
        return DtlsStatus::NotConnected;
    }
    auto *ssl = static_cast<WOLFSSL *>(ssl_);

    int rc = (config_.role == DtlsRole::Server) ? wolfSSL_accept(ssl) : wolfSSL_connect(ssl);
    if (rc != WOLFSSL_SUCCESS) {
        return DtlsStatus::HandshakeFailed; // includes a PSK mismatch
    }
    established_ = true;
    return DtlsStatus::Ok;
}

#endif // FUSE_PROTO_WITH_DTLS

// --- Shared data path ----------------------------------------------------
//
// With encryption off these are plain socket operations, so callers use a
// single code path regardless of policy.

DtlsStatus DtlsSession::send(const uint8_t *data, size_t len) {
    if (!config_.encryption_required) {
        if (socket_ == nullptr) return DtlsStatus::NotConnected;
        return socket_->send_to(data, len, peer_) ? DtlsStatus::Ok : DtlsStatus::IoError;
    }
#if FUSE_PROTO_WITH_DTLS
    if (!established_ || ssl_ == nullptr) return DtlsStatus::NotConnected;
    int rc = wolfSSL_write(static_cast<WOLFSSL *>(ssl_), data, static_cast<int>(len));
    return (rc == static_cast<int>(len)) ? DtlsStatus::Ok : DtlsStatus::IoError;
#else
    (void)data;
    (void)len;
    return DtlsStatus::Unsupported;
#endif
}

DtlsStatus DtlsSession::recv(uint8_t *buf, size_t buf_cap, size_t *out_len) {
    if (!config_.encryption_required) {
        if (socket_ == nullptr) return DtlsStatus::NotConnected;
        return socket_->recv_from(buf, buf_cap, out_len, nullptr) ? DtlsStatus::Ok
                                                                  : DtlsStatus::IoError;
    }
#if FUSE_PROTO_WITH_DTLS
    if (!established_ || ssl_ == nullptr) return DtlsStatus::NotConnected;
    int rc = wolfSSL_read(static_cast<WOLFSSL *>(ssl_), buf, static_cast<int>(buf_cap));
    if (rc <= 0) return DtlsStatus::IoError;
    *out_len = static_cast<size_t>(rc);
    return DtlsStatus::Ok;
#else
    (void)buf;
    (void)buf_cap;
    (void)out_len;
    return DtlsStatus::Unsupported;
#endif
}

} // namespace fuse::proto
