#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "fuse/http.h"
#include "test_ports.hpp"

namespace {

fuse_config make_cfg(uint16_t port) {
    fuse_config cfg;
    fuse_config_init(&cfg);
    cfg.bind_address = "127.0.0.1";
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.timeout_ms = 3000;
    return cfg;
}

// Runs fuse_http_serve with `handler` on its own thread for the lifetime of
// the object.
struct TestServer {
    fuse_config cfg;
    volatile int stop = 0;
    fuse_status result = FUSE_ERR_INTERNAL;
    std::thread th;

    TestServer(fuse_http_handler handler, void *user_data)
        : cfg(make_cfg(fuse_test::free_port_block(1))) {
        th = std::thread([this, handler, user_data] {
            result = fuse_http_serve(&cfg, handler, user_data, &stop);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ~TestServer() {
        stop = 1;
        th.join();
    }
};

fuse_status echo_handler(const fuse_http_request *req, fuse_http_response *resp, void *) {
    resp->status = 200;
    std::string body = std::string(req->method) + " " + req->path;
    size_t n = 0;
    if (const char *v = fuse_http_get_header(req->headers, "X-Test", &n)) {
        body += " " + std::string(v, n);
    }
    resp->body = std::malloc(body.size());
    std::memcpy(resp->body, body.data(), body.size());
    resp->body_len = body.size();
    return FUSE_OK;
}

fuse_status get(const fuse_config &cfg, const char *method, const char *path, const char *headers,
                fuse_http_response *resp) {
    fuse_http_request req{};
    req.method = method;
    req.path = path;
    req.headers = headers;
    return fuse_http_fetch(&cfg, &req, resp, 3000);
}

} // namespace

TEST(Http, RoundTrip) {
    TestServer srv(echo_handler, nullptr);
    fuse_http_response resp;
    ASSERT_EQ(get(srv.cfg, "GET", "/hello?x=1", "X-Test: yes\r\n", &resp), FUSE_OK);
    EXPECT_EQ(resp.status, 200);
    EXPECT_STREQ(resp.reason, "OK");
    EXPECT_EQ(std::string(static_cast<char *>(resp.body), resp.body_len), "GET /hello?x=1 yes");
    fuse_http_response_free(&resp);
}

// ROADMAP #29: CR/LF (or other bytes outside the grammar) in a method,
// path or header would inject header lines; such a request is refused
// before anything is sent.
TEST(Http, RequestInjectionIsRefused) {
    const fuse_config cfg = make_cfg(1); // never contacted
    fuse_http_response resp;
    EXPECT_EQ(get(cfg, "GET", "/a\r\nX-Evil: 1", nullptr, &resp), FUSE_ERR_CONFIG);
    EXPECT_EQ(get(cfg, "GET", "/a b", nullptr, &resp), FUSE_ERR_CONFIG);
    EXPECT_EQ(get(cfg, "GE\nT", "/", nullptr, &resp), FUSE_ERR_CONFIG);
    EXPECT_EQ(get(cfg, "GET", "/", "X-A: 1\r\nX-B: 2\nX-Evil: 1\r\n", &resp), FUSE_ERR_CONFIG);
    EXPECT_EQ(get(cfg, "GET", "/", "X-A: 1", &resp), FUSE_ERR_CONFIG); // no trailing CRLF
    EXPECT_EQ(get(cfg, "GET", "/", "No colon\r\n", &resp), FUSE_ERR_CONFIG);
    EXPECT_EQ(get(cfg, "GET", "/", "X-A: \r\n\r\nGET /smuggled HTTP/1.1\r\n", &resp),
              FUSE_ERR_CONFIG);
}

fuse_status injecting_handler(const fuse_http_request *, fuse_http_response *resp, void *) {
    resp->status = 200;
    const char h[] = "Set-Cookie: a=1\r\nX-Evil: 1\nInjected: yes\r\n";
    resp->headers = static_cast<char *>(std::malloc(sizeof(h)));
    std::memcpy(resp->headers, h, sizeof(h));
    return FUSE_OK;
}

// A handler that builds an invalid response (say, a header value copied
// from untrusted input) gets a 500 sent instead, not the injected bytes.
TEST(Http, InvalidHandlerResponseBecomes500) {
    TestServer srv(injecting_handler, nullptr);
    fuse_http_response resp;
    ASSERT_EQ(get(srv.cfg, "GET", "/", nullptr, &resp), FUSE_OK);
    EXPECT_EQ(resp.status, 500);
    EXPECT_EQ(fuse_http_get_header(resp.headers, "Injected", nullptr), nullptr);
    fuse_http_response_free(&resp);
}

fuse_status long_reason_handler(const fuse_http_request *, fuse_http_response *resp, void *) {
    resp->status = 299;
    const std::string reason(200, 'r');
    resp->reason = static_cast<char *>(std::malloc(reason.size() + 1));
    std::memcpy(resp->reason, reason.c_str(), reason.size() + 1);
    return FUSE_OK;
}

// A reason phrase longer than the old 64-byte status-line buffer arrives
// whole instead of truncated.
TEST(Http, LongReasonPhraseIsNotTruncated) {
    TestServer srv(long_reason_handler, nullptr);
    fuse_http_response resp;
    ASSERT_EQ(get(srv.cfg, "GET", "/", nullptr, &resp), FUSE_OK);
    EXPECT_EQ(resp.status, 299);
    EXPECT_EQ(std::string(resp.reason), std::string(200, 'r'));
    fuse_http_response_free(&resp);
}

struct SlowState {
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
};

fuse_status slow_handler(const fuse_http_request *, fuse_http_response *resp, void *ud) {
    auto *st = static_cast<SlowState *>(ud);
    st->entered = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    resp->status = 204;
    st->finished = true;
    return FUSE_OK;
}

// fuse_http_serve must not return while a worker is still running the
// caller's handler with the caller's user_data.
TEST(Http, ServeWaitsForInFlightHandlers) {
    SlowState st;
    fuse_http_response resp;
    fuse_status fetched = FUSE_ERR_INTERNAL;
    {
        TestServer srv(slow_handler, &st);
        std::thread client([&] { fetched = get(srv.cfg, "GET", "/", nullptr, &resp); });
        while (!st.entered) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        srv.stop = 1;
        srv.th.join();
        EXPECT_TRUE(st.finished) << "serve returned while a handler was still running";
        EXPECT_EQ(srv.result, FUSE_OK);
        srv.th = std::thread([] {}); // the destructor joins it
        client.join();
    }
    EXPECT_EQ(fetched, FUSE_OK);
    EXPECT_EQ(resp.status, 204);
    fuse_http_response_free(&resp);
}
