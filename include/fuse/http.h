#ifndef FUSE_HTTP_H
#define FUSE_HTTP_H

/*
 * HTTP over Fuse, one request, one response, one Fuse message each way.
 *
 * Discovery (how a client learns an origin speaks this): an ordinary HTTPS
 * response advertises it via Alt-Svc, token "hfuse-00" (see
 * internal-design/http-over-fuse.md in this repo for why that token and
 * not a new URI scheme or a literal "HTTP/3.1"). Nothing in this header
 * deals with that negotiation, it is the request/response mechanics once
 * a client has already decided to open a Fuse connection to a given
 * host:port.
 *
 * Wire format is plain HTTP/1.1 request/response syntax (request-line or
 * status-line, headers, blank line, body), not reinvented, since
 * fuse_send()/fuse_recv() already give exact message boundaries for free,
 * which is the one thing that makes HTTP/1.1 framing hard over a byte
 * stream. One fuse_send() carries the whole request; one fuse_recv()
 * carries the whole response. No chunked transfer-encoding, no
 * keep-alive/pipelining: a connection is good for exactly one exchange in
 * this first version. See README / internal-design for what a v2 with
 * multiple requests per connection would build on
 * (fuse/mux_transfer.hpp's N-logical-streams-over-one-socket).
 *
 * Headers are exposed as the raw "Name: value\r\n..." block they're
 * serialized as, fuse_http_get_header() does a linear case-insensitive
 * scan over that block rather than this header inventing a list/map type
 * for what's usually a handful of headers.
 *
 * Platform: Linux (inherits fuse/sdk.h's constraint).
 */

#include <stddef.h>

#include "fuse/sdk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The Alt-Svc / ALPN-style identifier for this mapping. See the file
 * comment above, versions the HTTP-over-Fuse mapping itself, not the
 * underlying Fuse wire protocol (fuse/proto/wire.hpp's own
 * kProtocolVersion). */
#define FUSE_HTTP_ALT_SVC_TOKEN "hfuse-00"

/* ---- Request/response ------------------------------------------------- */

typedef struct fuse_http_request {
    const char *method;  /* "GET", "POST", ... (no copy taken; must outlive
                           * the fuse_http_fetch() call) */
    const char *path;    /* "/foo/bar?query", no scheme/host (those come
                           * from fuse_config) */
    const char *headers;  /* "Name: value\r\nName2: value2\r\n", or NULL/""
                           * for none. No trailing blank line, that's
                           * added automatically. */
    const void *body;    /* NULL if body_len == 0 */
    size_t body_len;
} fuse_http_request;

typedef struct fuse_http_response {
    int status;          /* 200, 404, ... */
    char *reason;        /* "OK", "Not Found", ... caller-owned, see
                           * fuse_http_response_free() */
    char *headers;       /* raw block, same format as the request's;
                           * caller-owned */
    void *body;          /* caller-owned */
    size_t body_len;
} fuse_http_response;

/* Fills every field with a safe empty/zero value. Always call this before
 * using a fuse_http_response you didn't get from fuse_http_fetch() (e.g.
 * one you're about to fill in and hand to a server handler). */
void fuse_http_response_init(fuse_http_response *resp);

/* Releases reason/headers/body. Safe to call on a zero-initialized or
 * already-freed response (idempotent). Does not free resp itself. */
void fuse_http_response_free(fuse_http_response *resp);

/* Case-insensitive lookup in a raw "Name: value\r\n..." header block (as
 * found in fuse_http_request::headers or fuse_http_response::headers).
 * Returns NULL if absent, with *out_len left untouched. On a match,
 * returns a pointer aliasing into `headers` (valid only as long as that
 * buffer is; copy it if you need to keep it past that) and sets *out_len
 * to the value's length, NOT NUL-terminated and not safe to strlen()
 * since it points into the middle of `headers`, not a standalone string.
 * Does not allocate. */
const char *fuse_http_get_header(const char *headers, const char *name,
                                 size_t *out_len);

/* ---- Client ------------------------------------------------------------ */

/* Connects to cfg->host:cfg->port, sends req, waits for the response, and
 * closes the connection (this version is one-exchange-per-connection; see
 * the file comment). On FUSE_OK, *out_resp is filled in and the caller
 * must eventually call fuse_http_response_free() on it. On any other
 * status, *out_resp is left zeroed (safe to free regardless).
 *
 * req->method, req->path and req->headers must follow the HTTP grammar:
 * a token method, a path of visible ASCII (no spaces), and zero or more
 * "Name: value\r\n" lines. Anything else (in particular a CR or LF that
 * would inject a header) is refused with FUSE_ERR_CONFIG before anything
 * is sent. A response whose status line or header block is malformed is
 * reported as FUSE_ERR_INTERNAL.
 *
 * timeout_ms > 0 is a budget for the whole exchange: the connect and every
 * wait inside the send are bounded by it, the response wait gets what is
 * left, and FUSE_ERR_TIMEOUT is returned once it is used up. (A send that
 * keeps making progress can run somewhat past it before that is noticed.)
 * <0: connect and send use cfg->timeout_ms, and the response is waited for
 * indefinitely. 0: the response is only polled for. */
fuse_status fuse_http_fetch(const fuse_config *cfg, const fuse_http_request *req,
                            fuse_http_response *out_resp, int timeout_ms);

/* ---- Server ------------------------------------------------------------ */

/* Called once per accepted connection with the request that connection
 * sent. Fill *out_resp (already zero-initialized by the caller) and
 * return FUSE_OK to send it; any other return closes the connection
 * without a response (the client's fuse_http_fetch() sees that as
 * whatever fuse_recv() reports, typically FUSE_ERR_CLOSED). A response
 * with a status outside 100..999, or a reason phrase or header block that
 * breaks the HTTP grammar (e.g. a value carrying CR/LF copied from the
 * request), is replaced with a bare 500 rather than sent. Requests that are
 * malformed never reach the handler; they get a 400. */
typedef fuse_status (*fuse_http_handler)(const fuse_http_request *req,
                                         fuse_http_response *out_resp,
                                         void *user_data);

/* Binds cfg->port and accepts connections on the calling thread (sdk.h
 * requires accept to happen from one thread at a time per listener), but
 * hands each accepted connection's request to handler on its own
 * worker thread, so one slow handler does not stall the accept loop or
 * other in-flight requests. Each connection has cfg->timeout_ms (default
 * 10 s) to send its request, and at most 256 are served at once; further
 * connections are closed until a worker frees up. (fuse_accept does not
 * authenticate clients, so these bounds are what keep idle connections
 * from holding threads indefinitely.)
 *
 * Returns once *stop_flag becomes non-zero (checked between accepts, so
 * this returns within about a second), or when fuse_listen()/fuse_accept()
 * fails, with that error. Before returning it waits for every in-flight
 * handler to finish, so handler and user_data only need to stay valid
 * until this returns. Pass NULL for stop_flag to run until an error. */
fuse_status fuse_http_serve(const fuse_config *cfg, fuse_http_handler handler,
                            void *user_data, const volatile int *stop_flag);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FUSE_HTTP_H */
