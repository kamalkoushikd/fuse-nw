// Implementation of fuse/http.h. See that header for the wire format and
// the one-exchange-per-connection scope of this first version.

#include "fuse/http.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    if (!cfg || !req || !out_resp || !req->method || !req->path) {
        return FUSE_ERR_CONFIG;
    }

    fuse_status err = FUSE_OK;
    fuse_conn *c = fuse_connect(cfg, &err);
    if (!c) return err;

    std::string wire;
    wire.append(req->method).append(" ").append(req->path).append(" HTTP/1.1\r\n");
    AppendHeadersAndBody(&wire, req->headers, req->body, req->body_len);

    err = fuse_send(c, wire.data(), wire.size());
    if (err != FUSE_OK) {
        fuse_close(c);
        return err;
    }

    void *raw = nullptr;
    size_t raw_len = 0;
    err = fuse_recv_alloc(c, &raw, &raw_len, timeout_ms);
    fuse_close(c);
    if (err != FUSE_OK) return err;

    ParsedMessage m = ParseMessage(static_cast<const char *>(raw), raw_len);
    if (!m.ok) {
        fuse_free(raw);
        return FUSE_ERR_INTERNAL;
    }

    // "HTTP/1.1 200 OK" -> skip the version token, parse the code, keep
    // the rest (trimmed of its own leading space) as the reason phrase.
    const char *line = m.first_line.c_str();
    const char *sp1 = std::strchr(line, ' ');
    int status = sp1 ? std::atoi(sp1 + 1) : 0;
    const char *reason = sp1 ? std::strchr(sp1 + 1, ' ') : nullptr;
    if (reason) {
        while (*reason == ' ') ++reason;
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

void ServeOneConnection(fuse_conn *c, fuse_http_handler handler, void *user_data) {
    void *raw = nullptr;
    size_t raw_len = 0;
    fuse_status err = fuse_recv_alloc(c, &raw, &raw_len, -1);
    if (err != FUSE_OK) {
        fuse_close(c);
        return;
    }

    ParsedMessage m = ParseMessage(static_cast<const char *>(raw), raw_len);
    if (!m.ok) {
        fuse_free(raw);
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
    const fuse_status handler_status = handler(&req, &resp, user_data);

    if (handler_status == FUSE_OK) {
        if (!resp.reason) resp.reason = DupStr(DefaultReason(resp.status));
        std::string wire;
        char status_line[64];
        std::snprintf(status_line, sizeof status_line, "HTTP/1.1 %d %s\r\n",
                     resp.status, resp.reason ? resp.reason : "");
        wire.append(status_line);
        AppendHeadersAndBody(&wire, resp.headers, resp.body, resp.body_len);
        fuse_send(c, wire.data(), wire.size());
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

    while (!stop_flag || *stop_flag == 0) {
        fuse_conn *c = fuse_accept(l, 1000, &err);
        if (!c) {
            if (err == FUSE_ERR_TIMEOUT) continue;
            break;
        }
        std::thread(ServeOneConnection, c, handler, user_data).detach();
    }

    fuse_listener_close(l);
    return FUSE_OK;
}

}  // extern "C"
