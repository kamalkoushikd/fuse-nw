// Implementation of fuse/http.h. See that header for the wire format and
// the one-exchange-per-connection scope of this first version.

#include "fuse/http.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

// Case-insensitive length-bounded compare, std::strncasecmp isn't portable
// (BSD/POSIX extension, not plain C/C++) and this repo's other SDK source
// avoids reaching for a new dependency over a five-line function.
bool EqualsIgnoreCase(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (a_len != b_len) return false;
    for (size_t i = 0; i < a_len; ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

char *DupLen(const char *data, size_t len) {
    char *out = static_cast<char *>(std::malloc(len + 1));
    if (!out) return nullptr;
    if (len) std::memcpy(out, data, len);
    out[len] = '\0';
    return out;
}

char *DupStr(const std::string &s) { return DupLen(s.data(), s.size()); }

// A received message, split into its three wire-format parts. `headers`
// and `body` point into the backing buffer this was parsed from; neither
// is NUL-terminated on its own (body especially, binary bodies are
// legal), so callers must use the paired *_len.
struct ParsedMessage {
    std::string first_line;  // owns a copy, parsing needs a NUL-terminated
                              // line and the backing buffer isn't one
    const char *headers = nullptr;
    size_t headers_len = 0;
    const void *body = nullptr;
    size_t body_len = 0;
    bool ok = false;
};

ParsedMessage ParseMessage(const char *data, size_t len) {
    ParsedMessage m;

    // Find "\r\n\r\n" in the *whole* message first, not after already
    // splitting off the first line: when there are zero headers, the
    // blank line is the first line's own \r\n immediately followed by
    // one more \r\n, searching only in what comes after the first line
    // would miss that (nothing left to pair the lone \r\n with).
    const char *blank = nullptr;
    for (size_t i = 0; i + 4 <= len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' &&
            data[i + 3] == '\n') {
            blank = data + i;
            break;
        }
    }
    if (!blank) return m;

    // [data, blank+2) is "first-line\r\n[header-line\r\n]*", ending in the
    // last header's own \r\n (or the first line's, if there are no
    // headers, both are just "a \r\n-terminated line").
    const char *prefix_end = blank + 2;
    const char *line_end =
        static_cast<const char *>(memchr(data, '\n', static_cast<size_t>(prefix_end - data)));
    if (!line_end || line_end == data || *(line_end - 1) != '\r') return m;

    m.first_line.assign(data, static_cast<size_t>(line_end - data) - 1);  // drop \r
    m.headers = line_end + 1;
    m.headers_len = static_cast<size_t>(prefix_end - m.headers);

    const char *body_start = blank + 4;
    m.body = body_start;
    m.body_len = len - static_cast<size_t>(body_start - data);
    m.ok = true;
    return m;
}

// Appends headers_block (already "Name: value\r\n..." per line, no
// trailing blank line, see fuse/http.h) plus the separating blank line
// and body to `out`.
void AppendHeadersAndBody(std::string *out, const char *headers_block,
                          const void *body, size_t body_len) {
    if (headers_block && *headers_block) out->append(headers_block);
    out->append("\r\n");
    if (body && body_len) out->append(static_cast<const char *>(body), body_len);
}

// --- Syntax checks (RFC 9110 / 9112) ------------------------------------
//
// Every caller-supplied piece that goes on the wire is checked before it
// is sent, and every received header block before it is handed out: a CR
// or LF inside a method, path, header value or reason phrase would let one
// party inject header lines (or a whole second message) into the stream.

// tchar: the characters allowed in a method and a header name.
bool IsTchar(unsigned char ch) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
        return true;
    }
    return std::strchr("!#$%&'*+-.^_`|~", ch) != nullptr && ch != '\0';
}

// field-vchar / SP / HTAB, plus obs-text: anything but CTLs (CR, LF, NUL...).
bool IsFieldChar(unsigned char ch) { return ch == '\t' || (ch >= 0x20 && ch != 0x7F); }

bool ValidToken(const char *s) {
    if (s == nullptr || *s == '\0') return false;
    for (; *s; ++s) {
        if (!IsTchar(static_cast<unsigned char>(*s))) return false;
    }
    return true;
}

// request-target: visible ASCII only, so no spaces and no CTLs.
bool ValidPath(const char *s) {
    if (s == nullptr || *s == '\0') return false;
    for (; *s; ++s) {
        const unsigned char ch = static_cast<unsigned char>(*s);
        if (ch <= 0x20 || ch >= 0x7F) return false;
    }
    return true;
}

bool ValidReason(const char *s) {
    if (s == nullptr) return true;
    for (; *s; ++s) {
        if (!IsFieldChar(static_cast<unsigned char>(*s))) return false;
    }
    return true;
}

// A header block: zero or more "name: value\r\n" lines, nothing else (the
// block must end in \r\n, or the blank line that ends the headers would
// be misplaced). `len` bytes at `p`.
bool ValidHeaderBlock(const char *p, size_t len) {
    size_t i = 0;
    while (i < len) {
        const size_t start = i;
        while (i < len && p[i] != '\r' && p[i] != '\n') ++i;
        if (i + 1 >= len || p[i] != '\r' || p[i + 1] != '\n') return false; // bare CR/LF, or no CRLF
        const size_t end = i;
        i += 2;
        size_t colon = start;
        while (colon < end && p[colon] != ':') ++colon;
        if (colon == start || colon == end) return false; // empty name, or no colon
        for (size_t k = start; k < colon; ++k) {
            if (!IsTchar(static_cast<unsigned char>(p[k]))) return false;
        }
        for (size_t k = colon + 1; k < end; ++k) {
            if (!IsFieldChar(static_cast<unsigned char>(p[k]))) return false;
        }
    }
    return true;
}

bool ValidHeaders(const char *headers) {
    return headers == nullptr || ValidHeaderBlock(headers, std::strlen(headers));
}

// "HTTP/1.x SP 3DIGIT [SP reason]": the status code, or 0 if the line isn't
// one. *reason points at the reason phrase (possibly empty).
int ParseStatusLine(const char *line, const char **reason) {
    if (std::strncmp(line, "HTTP/1.", 7) != 0 || line[7] < '0' || line[7] > '9' ||
        line[8] != ' ') {
        return 0;
    }
    const char *d = line + 9;
    for (int k = 0; k < 3; ++k) {
        if (d[k] < '0' || d[k] > '9') return 0;
    }
    if (d[3] != '\0' && d[3] != ' ') return 0;
    const int status = (d[0] - '0') * 100 + (d[1] - '0') * 10 + (d[2] - '0');
    if (status < 100) return 0;
    *reason = d[3] == ' ' ? d + 4 : d + 3;
    return status;
}

// Default reason phrases for the status codes this project's own examples
// and tests are likely to actually use. Anything else falls back to
// "Unknown" rather than this growing into a full IANA status registry,
// callers who care about a specific phrase should set fuse_http_response
// fields directly instead of relying on this table.
const char *DefaultReason(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

}  // namespace

extern "C" {

void fuse_http_response_init(fuse_http_response *resp) {
    if (!resp) return;
    resp->status = 0;
    resp->reason = nullptr;
    resp->headers = nullptr;
    resp->body = nullptr;
    resp->body_len = 0;
}

void fuse_http_response_free(fuse_http_response *resp) {
    if (!resp) return;
    std::free(resp->reason);
    std::free(resp->headers);
    std::free(resp->body);
    fuse_http_response_init(resp);
}

const char *fuse_http_get_header(const char *headers, const char *name,
                                 size_t *out_len) {
    if (!headers || !name) return nullptr;
    const size_t name_len = std::strlen(name);
    const char *p = headers;
    const char *end = headers + std::strlen(headers);

    while (p < end) {
        const char *line_end = static_cast<const char *>(
            memchr(p, '\n', static_cast<size_t>(end - p)));
        const char *line_stop = line_end ? line_end - 1 : end;  // before \r
        const char *colon = static_cast<const char *>(
            memchr(p, ':', static_cast<size_t>(line_stop - p)));
        if (colon) {
            const size_t key_len = static_cast<size_t>(colon - p);
            if (EqualsIgnoreCase(p, key_len, name, name_len)) {
                const char *val = colon + 1;
                while (val < line_stop && *val == ' ') ++val;
                if (out_len) *out_len = static_cast<size_t>(line_stop - val);
                return val;
            }
        }
        if (!line_end) break;
        p = line_end + 1;
    }
    return nullptr;
}

fuse_status fuse_http_fetch(const fuse_config *cfg, const fuse_http_request *req,
                            fuse_http_response *out_resp, int timeout_ms) {
    if (out_resp) fuse_http_response_init(out_resp);
    if (!cfg || !req || !out_resp || !ValidToken(req->method) || !ValidPath(req->path) ||
        !ValidHeaders(req->headers) || (req->body == nullptr && req->body_len != 0)) {
        return FUSE_ERR_CONFIG;
    }

    // timeout_ms > 0 is a budget for the whole exchange: the connect, and
    // every wait inside the send, are bounded by it, and the response wait
    // gets whatever is left. (<0 leaves connect/send at cfg->timeout_ms and
    // waits for the response indefinitely; 0 polls for the response.)
    const auto start = std::chrono::steady_clock::now();
    const auto remaining_ms = [&]() -> int {
        const auto used = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();
        return used >= timeout_ms ? 0 : static_cast<int>(timeout_ms - used);
    };
    fuse_config conn_cfg = *cfg;
    if (timeout_ms > 0) conn_cfg.timeout_ms = static_cast<uint32_t>(timeout_ms);

    fuse_status err = FUSE_OK;
    fuse_conn *c = fuse_connect(&conn_cfg, &err);
    if (!c) return err;

    std::string wire;
    wire.append(req->method).append(" ").append(req->path).append(" HTTP/1.1\r\n");
    AppendHeadersAndBody(&wire, req->headers, req->body, req->body_len);

    err = fuse_send(c, wire.data(), wire.size());
    if (err == FUSE_OK && timeout_ms > 0 && remaining_ms() == 0) err = FUSE_ERR_TIMEOUT;
    if (err != FUSE_OK) {
        fuse_close(c);
        return err;
    }

    void *raw = nullptr;
    size_t raw_len = 0;
    err = fuse_recv_alloc(c, &raw, &raw_len, timeout_ms > 0 ? remaining_ms() : timeout_ms);
    fuse_close(c);
    if (err != FUSE_OK) return err;

    ParsedMessage m = ParseMessage(static_cast<const char *>(raw), raw_len);
    // "HTTP/1.1 200 OK": the version, a three-digit code, the reason
    // phrase. Anything else -- or a header block with stray CR/LF -- is a
    // malformed response, not something to hand to the caller.
    const char *reason = nullptr;
    const int status = m.ok ? ParseStatusLine(m.first_line.c_str(), &reason) : 0;
    if (status == 0 || !ValidReason(reason) || !ValidHeaderBlock(m.headers, m.headers_len)) {
        fuse_free(raw);
        return FUSE_ERR_INTERNAL;
    }

    out_resp->status = status;
    out_resp->reason = DupStr(reason ? reason : "");
    out_resp->headers = DupLen(m.headers, m.headers_len);
    out_resp->body = DupLen(static_cast<const char *>(m.body), m.body_len);
    if (out_resp->body) out_resp->body_len = m.body_len;

    fuse_free(raw);

    if (!out_resp->reason || !out_resp->headers || !out_resp->body) {
        fuse_http_response_free(out_resp);
        return FUSE_ERR_INTERNAL;
    }
    return FUSE_OK;
}

namespace {

// Upper bound on connections being served at once. fuse_accept does not
// authenticate clients, so without a cap a flood of connections that never
// send a request would each hold a thread (ROADMAP #4); past the cap, new
// connections are closed straight away.
constexpr int kMaxInFlight = 256;
// How long a worker waits for its connection's request.
constexpr int kDefaultRequestWaitMs = 10000;

// Worker bookkeeping shared by fuse_http_serve and its workers, so serve can
// wait for them before returning (they use the caller's handler/user_data).
struct Workers {
    std::mutex mu;
    std::condition_variable cv;
    int active = 0;
};

void SendSimpleResponse(fuse_conn *c, int status, const char *reason) {
    std::string wire = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n\r\n";
    fuse_send(c, wire.data(), wire.size());
}

void ServeOneConnection(fuse_conn *c, fuse_http_handler handler, void *user_data,
                        int request_wait_ms) {
    void *raw = nullptr;
    size_t raw_len = 0;
    fuse_status err = fuse_recv_alloc(c, &raw, &raw_len, request_wait_ms);
    if (err != FUSE_OK) {
        fuse_close(c);
        return;
    }

    ParsedMessage m = ParseMessage(static_cast<const char *>(raw), raw_len);
    if (!m.ok || !ValidHeaderBlock(m.headers, m.headers_len)) {
        fuse_free(raw);
        SendSimpleResponse(c, 400, "Bad Request");
        fuse_close(c);
        return;
    }

    // "GET /path HTTP/1.1" -> method up to the first space, path between
    // that and the " HTTP/" version marker (searched from the end, since
    // the version token is always last and a path is never expected to
    // contain " HTTP/" itself).
    const char *line = m.first_line.c_str();
    const char *sp1 = std::strchr(line, ' ');
    std::string method = sp1 ? std::string(line, sp1) : std::string(line);
    std::string path;
    if (sp1) {
        const char *version_marker = std::strstr(sp1 + 1, " HTTP/");
        path = version_marker ? std::string(sp1 + 1, version_marker)
                               : std::string(sp1 + 1);
    }

    std::string headers_copy(m.headers, m.headers_len);
    std::string body_copy(static_cast<const char *>(m.body), m.body_len);
    fuse_free(raw);

    fuse_http_request req;
    req.method = method.c_str();
    req.path = path.c_str();
    req.headers = headers_copy.c_str();
    req.body = body_copy.empty() ? nullptr : body_copy.data();
    req.body_len = body_copy.size();

    fuse_http_response resp;
    fuse_http_response_init(&resp);
    if (!ValidToken(method.c_str()) || !ValidPath(path.c_str())) {
        SendSimpleResponse(c, 400, "Bad Request");
        fuse_close(c);
        return;
    }
    const fuse_status handler_status = handler(&req, &resp, user_data);

    if (handler_status == FUSE_OK) {
        if (!resp.reason) resp.reason = DupStr(DefaultReason(resp.status));
        // A response the handler built with an invalid status, reason or
        // header block (e.g. a value carrying CR/LF from untrusted input)
        // is not sent as is: that would be header injection.
        if (resp.status < 100 || resp.status > 999 || !ValidReason(resp.reason) ||
            !ValidHeaders(resp.headers) || (resp.body == nullptr && resp.body_len != 0)) {
            SendSimpleResponse(c, 500, "Internal Server Error");
        } else {
            // Built as a string, not into a fixed buffer, so a long reason
            // phrase is sent whole instead of silently truncated.
            std::string wire = "HTTP/1.1 " + std::to_string(resp.status) + " " +
                               (resp.reason ? resp.reason : "") + "\r\n";
            AppendHeadersAndBody(&wire, resp.headers, resp.body, resp.body_len);
            fuse_send(c, wire.data(), wire.size());
        }
    }

    fuse_http_response_free(&resp);
    fuse_close(c);
}

}  // namespace

fuse_status fuse_http_serve(const fuse_config *cfg, fuse_http_handler handler,
                            void *user_data, const volatile int *stop_flag) {
    if (!cfg || !handler) return FUSE_ERR_CONFIG;

    fuse_status err = FUSE_OK;
    fuse_listener *l = fuse_listen(cfg, &err);
    if (!l) return err;

    const int request_wait_ms =
        cfg->timeout_ms ? static_cast<int>(cfg->timeout_ms) : kDefaultRequestWaitMs;
    Workers workers;
    fuse_status result = FUSE_OK;
    while (!stop_flag || *stop_flag == 0) {
        fuse_conn *c = fuse_accept(l, 1000, &err);
        if (!c) {
            if (err == FUSE_ERR_TIMEOUT) continue;
            result = err; // a real accept failure is reported, not hidden
            break;
        }
        {
            std::lock_guard<std::mutex> lk(workers.mu);
            if (workers.active >= kMaxInFlight) {
                fuse_close(c); // over capacity: shed this connection
                continue;
            }
            ++workers.active;
        }
        std::thread([c, handler, user_data, request_wait_ms, &workers] {
            ServeOneConnection(c, handler, user_data, request_wait_ms);
            std::lock_guard<std::mutex> lk(workers.mu);
            --workers.active;
            workers.cv.notify_all();
        }).detach();
    }

    fuse_listener_close(l);
    // The workers call the caller's handler with the caller's user_data, so
    // don't return (and let the caller free them) until every one is done.
    std::unique_lock<std::mutex> lk(workers.mu);
    workers.cv.wait(lk, [&] { return workers.active == 0; });
    return result;
}

}  // extern "C"
