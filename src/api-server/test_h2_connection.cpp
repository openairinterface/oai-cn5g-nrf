// test_h2_connection.cpp
// Unit tests for h2::Connection (Task 4.1).
//
// Tests:
//   1. ValidRequestDispatch   — GET routed to handler, 200 returned.
//   2. BodyAccumulation       — POST body fully received in handler.
//   3. HPACKBomb              — headers > 64 KB → RST_STREAM.
//   4. UnregisteredPath404    — unmatched path → 404 response.
//   5. IdleTimeout            — close() (what timer fires) → CLOSED state.
//   6. DoubleCloseSafety      — second close() is a no-op, no crash.
//   7. BodyLimit              — body > 1 MB → RST_STREAM.
//   8. PartialWriteRecovery   — large (64 KB) response fully delivered.
//
// Build note: must be compiled with the full production sources and -lnghttp2.
// See the required aggregate command in the task definition.

#include <gtest/gtest.h>
#include "h2_connection.hpp"
#include "h2_request.hpp"
#include "h2_response.hpp"
#include "h2_event_loop.hpp"
#include "h2_router.hpp"
#include "h2_constants.hpp"

#include <nghttp2/nghttp2.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Shared helpers
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// Spin-wait until pred() or timeout_ms elapses.
template<typename Pred>
static bool wait_for(Pred pred, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// H2TestPeer — minimal synchronous nghttp2 HTTP/2 client.
//
// Operates on the client side of a socketpair.  The socket is non-blocking;
// pump() drains whatever is readable each invocation.  Uses the mem-based
// nghttp2 API (no send callback) so flush() is explicit.
// ─────────────────────────────────────────────────────────────────────────────
struct ResponseData {
    int         status   = 0;
    std::string body;
    bool        complete = false;
    bool        reset    = false;
};

class H2TestPeer {
 public:
    explicit H2TestPeer(int fd) : fd_(fd) {
        // Non-blocking so pump() doesn't block the test thread.
        int fl = ::fcntl(fd_, F_GETFL, 0);
        ::fcntl(fd_, F_SETFL, fl | O_NONBLOCK);

        nghttp2_session_callbacks* cbs;
        nghttp2_session_callbacks_new(&cbs);
        nghttp2_session_callbacks_set_on_header_callback    (cbs, on_header_cb);
        nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, on_frame_cb);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, on_data_cb);
        nghttp2_session_callbacks_set_on_stream_close_callback   (cbs, on_close_cb);
        nghttp2_session_client_new(&session_, cbs, this);
        nghttp2_session_callbacks_del(cbs);

        // Queue the mandatory client SETTINGS frame.
        nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, nullptr, 0);
        flush();
    }

    ~H2TestPeer() {
        if (session_) nghttp2_session_del(session_);
    }

    // Drain nghttp2's outbound queue to the socket.
    void flush() {
        const uint8_t* data;
        ssize_t n;
        while ((n = nghttp2_session_mem_send(session_, &data)) > 0) {
            size_t off = 0;
            while (off < static_cast<size_t>(n)) {
                ssize_t r = ::send(fd_, data + off,
                                   static_cast<size_t>(n) - off, MSG_NOSIGNAL);
                if (r <= 0) return;     // EAGAIN or peer closed — stop
                off += static_cast<size_t>(r);
            }
        }
    }

    // Read from socket into nghttp2, then flush any generated replies.
    void pump() {
        uint8_t buf[16384];
        ssize_t n;
        while ((n = ::recv(fd_, buf, sizeof(buf), 0)) > 0) {
            nghttp2_session_mem_recv(session_, buf, static_cast<size_t>(n));
            flush();
        }
    }

    // True if nghttp2 still has frames to send (e.g. body DATA pending).
    bool want_write() const {
        return nghttp2_session_want_write(session_) != 0;
    }

    // Submit a GET request.  Returns stream_id > 0 on success.
    int32_t get(const std::string& path) {
        nghttp2_nv nva[] = {
            {(uint8_t*)":method",    (uint8_t*)"GET",       7,  3,           NGHTTP2_NV_FLAG_NONE},
            {(uint8_t*)":path",      (uint8_t*)path.c_str(),5,  path.size(), NGHTTP2_NV_FLAG_NONE},
            {(uint8_t*)":scheme",    (uint8_t*)"http",      7,  4,           NGHTTP2_NV_FLAG_NONE},
            {(uint8_t*)":authority", (uint8_t*)"localhost", 10, 9,           NGHTTP2_NV_FLAG_NONE},
        };
        int32_t id = nghttp2_submit_request(session_, nullptr, nva, 4,
                                            nullptr, nullptr);
        flush();
        return id;
    }

    // Submit a GET with additional custom headers (for HPACK-bomb test).
    int32_t get_with_extra_headers(const std::string& path,
                                   const std::vector<nghttp2_nv>& extra) {
        std::vector<nghttp2_nv> nva = {
            {(uint8_t*)":method",    (uint8_t*)"GET",       7,  3,           NGHTTP2_NV_FLAG_NONE},
            {(uint8_t*)":path",      (uint8_t*)path.c_str(),5,  path.size(), NGHTTP2_NV_FLAG_NONE},
            {(uint8_t*)":scheme",    (uint8_t*)"http",      7,  4,           NGHTTP2_NV_FLAG_NONE},
            {(uint8_t*)":authority", (uint8_t*)"localhost", 10, 9,           NGHTTP2_NV_FLAG_NONE},
        };
        for (const auto& h : extra) nva.push_back(h);
        int32_t id = nghttp2_submit_request(session_, nullptr,
                                            nva.data(), nva.size(),
                                            nullptr, nullptr);
        flush();
        return id;
    }

    // Submit a POST with a request body.
    int32_t post(const std::string& path, std::string body) {
        body_   = std::move(body);
        boff_   = 0;
        nghttp2_nv nva[] = {
            {(uint8_t*)":method",    (uint8_t*)"POST",      7,  4,           NGHTTP2_NV_FLAG_NONE},
            {(uint8_t*)":path",      (uint8_t*)path.c_str(),5,  path.size(), NGHTTP2_NV_FLAG_NONE},
            {(uint8_t*)":scheme",    (uint8_t*)"http",      7,  4,           NGHTTP2_NV_FLAG_NONE},
            {(uint8_t*)":authority", (uint8_t*)"localhost", 10, 9,           NGHTTP2_NV_FLAG_NONE},
        };
        nghttp2_data_provider prd{};
        prd.source.ptr   = this;
        prd.read_callback = body_read_cb;
        int32_t id = nghttp2_submit_request(session_, nullptr, nva, 4,
                                            &prd, nullptr);
        flush();
        return id;
    }

    // Poll until stream id completes or resets (or timeout).
    bool wait_done(int32_t id, int ms = 2000) {
        return wait_for([&] {
            pump();
            flush();
            auto it = responses_.find(id);
            return it != responses_.end() &&
                   (it->second.complete || it->second.reset);
        }, ms);
    }

    ResponseData* response(int32_t id) {
        auto it = responses_.find(id);
        return it != responses_.end() ? &it->second : nullptr;
    }

 private:
    // ---- nghttp2 callbacks ----

    static int on_header_cb(nghttp2_session*, const nghttp2_frame* f,
                            const uint8_t* n, size_t nl,
                            const uint8_t* v, size_t vl,
                            uint8_t, void* ud) {
        auto* self = static_cast<H2TestPeer*>(ud);
        if (f->hd.type != NGHTTP2_HEADERS) return 0;
        std::string name(reinterpret_cast<const char*>(n), nl);
        if (name == ":status") {
            std::string val(reinterpret_cast<const char*>(v), vl);
            self->responses_[f->hd.stream_id].status = std::stoi(val);
        }
        return 0;
    }

    static int on_frame_cb(nghttp2_session*, const nghttp2_frame* f, void* ud) {
        auto* self = static_cast<H2TestPeer*>(ud);
        // A HEADERS frame with END_STREAM marks a header-only response done.
        if (f->hd.type == NGHTTP2_HEADERS &&
            (f->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
            self->responses_[f->hd.stream_id].complete = true;
        }
        return 0;
    }

    static int on_data_cb(nghttp2_session*, uint8_t flags,
                          int32_t sid, const uint8_t* d, size_t l, void* ud) {
        auto* self = static_cast<H2TestPeer*>(ud);
        self->responses_[sid].body.append(reinterpret_cast<const char*>(d), l);
        if (flags & NGHTTP2_FLAG_END_STREAM) {
            self->responses_[sid].complete = true;
        }
        return 0;
    }

    static int on_close_cb(nghttp2_session*, int32_t sid,
                           uint32_t err, void* ud) {
        auto* self = static_cast<H2TestPeer*>(ud);
        self->responses_[sid].complete = true;
        if (err != 0) self->responses_[sid].reset = true;
        return 0;
    }

    static ssize_t body_read_cb(nghttp2_session*, int32_t,
                                uint8_t* buf, size_t len,
                                uint32_t* flags,
                                nghttp2_data_source*, void* ud) {
        auto* self = static_cast<H2TestPeer*>(ud);
        size_t rem = self->body_.size() - self->boff_;
        size_t n   = (rem < len) ? rem : len;
        if (n > 0) {
            std::memcpy(buf, self->body_.data() + self->boff_, n);
            self->boff_ += n;
        }
        if (self->boff_ >= self->body_.size()) {
            *flags |= NGHTTP2_DATA_FLAG_EOF;
        }
        return static_cast<ssize_t>(n);
    }

    int                             fd_;
    nghttp2_session*                session_ = nullptr;
    std::map<int32_t, ResponseData> responses_;
    std::string                     body_;
    size_t                          boff_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// ConnectionFixture — server-side EventLoop + Connection over a socketpair.
// ─────────────────────────────────────────────────────────────────────────────
class ConnectionFixture : public ::testing::Test {
 protected:
    void SetUp() override {
        int sv[2];
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        srv_fd_ = sv[0];
        cli_fd_ = sv[1];
    }

    // Call after setting up the router.  Creates the Connection and starts
    // the EventLoop thread.
    void start_server() {
        conn_ = std::make_shared<h2::Connection>(srv_fd_, loop_, router_);
        ASSERT_EQ(conn_->start(), 0);
        loop_.arm();
        t_ = std::thread([this] { loop_.run(); });
    }

    void stop_server() {
        if (loop_.is_running()) {
            loop_.post([this] {
                if (conn_) conn_->close();
                loop_.stop();
            });
        }
        if (t_.joinable()) t_.join();
    }

    void TearDown() override {
        stop_server();
        if (cli_fd_ >= 0) { ::close(cli_fd_); cli_fd_ = -1; }
        conn_.reset();
    }

    h2::EventLoop   loop_;
    h2::Router      router_;
    int             srv_fd_ = -1;
    int             cli_fd_ = -1;
    std::shared_ptr<h2::Connection> conn_;
    std::thread     t_;
};

}  // namespace


// ═══════════════════════════════════════════════════════════════════════════
// Test 1: ValidRequestDispatch
// A GET request to a registered path reaches the handler.
// The handler replies 200 with a body; the client receives both.
// ═══════════════════════════════════════════════════════════════════════════
TEST_F(ConnectionFixture, ValidRequestDispatch) {
    std::atomic<bool> handler_called{false};

    router_.handle("/health", [&](const h2::Request& req, h2::Response& res) {
        handler_called.store(true, std::memory_order_release);
        EXPECT_EQ(req.uri().path, "/health");
        res.write_head(200);
        res.end("ok");
    });

    start_server();

    H2TestPeer client(cli_fd_);
    int32_t id = client.get("/health");
    ASSERT_GT(id, 0);

    EXPECT_TRUE(client.wait_done(id, 3000)) << "timed out waiting for response";

    auto* r = client.response(id);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->status, 200);
    EXPECT_EQ(r->body, "ok");
    EXPECT_FALSE(r->reset);
    EXPECT_TRUE(handler_called.load(std::memory_order_acquire));
}


// ═══════════════════════════════════════════════════════════════════════════
// Test 2: BodyAccumulation
// A POST request with a 512-byte body is fully accumulated in the Connection
// before dispatch_request() is called.  The handler can read the complete body.
// ═══════════════════════════════════════════════════════════════════════════
TEST_F(ConnectionFixture, BodyAccumulation) {
    const std::string kPayload(512, 'B');
    std::string received_body;
    std::atomic<bool> handler_called{false};

    router_.handle("/echo", [&](const h2::Request& req, h2::Response& res) {
        received_body = req.body();
        handler_called.store(true, std::memory_order_release);
        res.write_head(200);
        res.end("ack");
    });

    start_server();

    H2TestPeer client(cli_fd_);
    int32_t id = client.post("/echo", kPayload);
    ASSERT_GT(id, 0);

    EXPECT_TRUE(client.wait_done(id, 3000)) << "timed out waiting for echo response";

    auto* r = client.response(id);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->status, 200);
    EXPECT_EQ(r->body, "ack");
    EXPECT_FALSE(r->reset);
    EXPECT_TRUE(handler_called.load(std::memory_order_acquire));
    EXPECT_EQ(received_body, kPayload);
}


// ═══════════════════════════════════════════════════════════════════════════
// Test 3: HPACKBomb
// A request carrying more than kMaxHeaderListSize (64 KB) of decompressed
// custom headers must be RST_STREAM'd by the server.
//
// Implementation note: the large-header request is submitted BEFORE the
// client processes the server's SETTINGS frame.  At that point the client's
// remote max_header_list_size is still UINT32_MAX (library default), so
// nghttp2_submit_request succeeds.  Once the EventLoop processes the payload
// it triggers the on_header_cb size guard.
// ═══════════════════════════════════════════════════════════════════════════
TEST_F(ConnectionFixture, HPACKBomb) {
    // Register a dummy handler (should never be called for the bomb stream).
    router_.handle("/bomb", [&](const h2::Request&, h2::Response& res) {
        res.write_head(200); res.end();
    });

    // Build 100 large custom headers — total decompressed size > 64 KB.
    // Each x-large-NNN (12 bytes) + 700-byte value = 712 bytes × 100 = 71 200 bytes.
    const std::string large_val(700, 'X');
    std::vector<nghttp2_nv> extra;
    extra.reserve(100);
    // Store names in a stable vector so the data pointers stay valid.
    std::vector<std::string> names;
    names.reserve(100);
    for (int i = 0; i < 100; ++i) {
        char name_buf[16];
        std::snprintf(name_buf, sizeof(name_buf), "x-large-%04d", i);
        names.push_back(name_buf);
    }
    for (int i = 0; i < 100; ++i) {
        extra.push_back({
            (uint8_t*)names[i].c_str(),
            (uint8_t*)large_val.c_str(),
            names[i].size(), large_val.size(),
            NGHTTP2_NV_FLAG_NO_INDEX  // Do not index in dynamic table
        });
    }

    // Create the client peer BEFORE starting the server so the HEADERS frame
    // is submitted while the client's remote max_header_list_size is unlimited.
    H2TestPeer client(cli_fd_);
    int32_t id = client.get_with_extra_headers("/bomb", extra);
    ASSERT_GT(id, 0);

    // Now start the server — it will process all buffered client data.
    start_server();

    // Server's on_header_cb should detect the oversized header block and
    // send RST_STREAM.  The client receives it via pump() inside wait_done().
    EXPECT_TRUE(client.wait_done(id, 3000)) << "timed out waiting for RST_STREAM";

    auto* r = client.response(id);
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->reset) << "expected RST_STREAM for oversized headers";
}


// ═══════════════════════════════════════════════════════════════════════════
// Test 4: UnregisteredPath404
// A GET request for a path that has no registered handler must receive a
// 404 response from the server via the dispatch_request() no-match path.
// ═══════════════════════════════════════════════════════════════════════════
TEST_F(ConnectionFixture, UnregisteredPath404) {
    // No handlers registered in router_ — every path is unmatched.
    start_server();

    H2TestPeer client(cli_fd_);
    int32_t id = client.get("/nonexistent/resource");
    ASSERT_GT(id, 0);

    EXPECT_TRUE(client.wait_done(id, 3000)) << "timed out waiting for 404 response";

    auto* r = client.response(id);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->status, 404);
    EXPECT_FALSE(r->reset);
}


// ═══════════════════════════════════════════════════════════════════════════
// Test 5: IdleTimeout (close mechanism)
// The idle timer that fires after kIdleTimeoutMs calls Connection::close().
// This test verifies the close() pathway directly: posts close() onto the
// loop thread (exactly what the timerfd callback does) and confirms the
// connection transitions to the CLOSED shutdown state.
//
// Note: the actual 30-second idle timer is not waited for — that would make
// the test suite take 30 s.  What is verified is the mechanism that the
// timer invokes.
// ═══════════════════════════════════════════════════════════════════════════
TEST_F(ConnectionFixture, IdleTimeout) {
    start_server();

    // Confirm the connection is live immediately after start().
    EXPECT_NE(conn_->shutdown_state(),
              h2::Connection::ShutdownState::CLOSED);

    // Simulate the idle timer firing: post close() to the owning loop thread.
    // This is exactly what the timerfd lambda in Connection::start() does.
    loop_.post([this]() {
        if (conn_) conn_->close();
    });

    // Wait for the connection to report CLOSED.
    EXPECT_TRUE(wait_for([&]() {
        return conn_->shutdown_state() ==
               h2::Connection::ShutdownState::CLOSED;
    }, 2000)) << "connection did not reach CLOSED state after close()";

    EXPECT_EQ(conn_->shutdown_state(), h2::Connection::ShutdownState::CLOSED);
}


// ═══════════════════════════════════════════════════════════════════════════
// Test 6: DoubleCloseSafety
// Calling Connection::close() a second time while the connection is already
// in the CLOSED state must be a silent no-op — no crash, no double-free.
// Also verifies that a weak_ptr captured in an epoll callback can be safely
// dereferenced (via lock()) after close() removes the owning shared_ptr from
// the EventLoop's connection map, as long as the test fixture's conn_ is
// still in scope.
// ═══════════════════════════════════════════════════════════════════════════
TEST_F(ConnectionFixture, DoubleCloseSafety) {
    start_server();

    // Capture a weak_ptr before closing — simulates an in-flight epoll cb.
    std::weak_ptr<h2::Connection> weak = conn_;

    // The connection must be live before our close() calls.
    EXPECT_NE(conn_->shutdown_state(), h2::Connection::ShutdownState::CLOSED);

    // Post two back-to-back close() calls on the owning loop thread.
    // The second call must be a no-op (closed_ guard).
    loop_.post([this]() {
        conn_->close();  // first close: full teardown
        conn_->close();  // second close: must return immediately
    });

    // Connection must reach CLOSED.
    EXPECT_TRUE(wait_for([&]() {
        return conn_->shutdown_state() ==
               h2::Connection::ShutdownState::CLOSED;
    }, 2000)) << "connection did not reach CLOSED state after double close()";

    EXPECT_EQ(conn_->shutdown_state(), h2::Connection::ShutdownState::CLOSED);

    // The test fixture's conn_ shared_ptr still keeps the object alive, so
    // weak.lock() must succeed even though the loop's map no longer holds it.
    EXPECT_NE(weak.lock(), nullptr)
        << "object must remain alive while conn_ shared_ptr is in scope";
}


// ═══════════════════════════════════════════════════════════════════════════
// Test 7: BodyLimit
// A POST request whose body exceeds kMaxRequestBodySize (1 MB) must be
// RST_STREAM'd by the server.  The server calls nghttp2_session_consume()
// after each DATA chunk so the flow-control window stays open and the full
// body can travel before the guard fires.
// ═══════════════════════════════════════════════════════════════════════════
TEST_F(ConnectionFixture, BodyLimit) {
    // Register a handler — it must NOT be invoked for the oversized stream.
    std::atomic<bool> handler_called{false};
    router_.handle("/upload", [&](const h2::Request&, h2::Response& res) {
        handler_called.store(true, std::memory_order_release);
        res.write_head(200);
        res.end("ok");
    });

    start_server();

    H2TestPeer client(cli_fd_);

    // Body is one byte over the 1 MB limit.
    const std::string large_body(h2::kMaxRequestBodySize + 1, 'X');
    int32_t id = client.post("/upload", large_body);
    ASSERT_GT(id, 0);

    // The server's on_data_chunk_recv_cb returns NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE
    // once the accumulated body would exceed kMaxRequestBodySize.  nghttp2 then
    // sends RST_STREAM (NGHTTP2_INTERNAL_ERROR) for the stream.
    EXPECT_TRUE(client.wait_done(id, 5000)) << "timed out waiting for RST_STREAM";

    auto* r = client.response(id);
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->reset) << "expected RST_STREAM for oversized request body";
    EXPECT_FALSE(handler_called.load(std::memory_order_acquire))
        << "handler must not be called for rejected oversized body";
}


// ═══════════════════════════════════════════════════════════════════════════
// Test 8: PartialWriteRecovery
// A handler that returns a 64 KB response body must have every byte
// delivered to the client intact, even though the body spans multiple
// fill_write_buf() / do_write() cycles (64 KB > kWriteBufSize when framing
// overhead is added).  The test verifies the write loop works correctly for
// multi-chunk delivery without requiring socket-buffer manipulation.
// ═══════════════════════════════════════════════════════════════════════════
TEST_F(ConnectionFixture, PartialWriteRecovery) {
    // 64 KB response body — four 16 KB DATA frames plus framing overhead,
    // which overflows one kWriteBufSize (65536 B) buffer slightly.
    const size_t kLargeBodySize = 65536;
    const std::string large_body(kLargeBodySize, 'R');

    router_.handle("/large", [&](const h2::Request&, h2::Response& res) {
        res.write_head(200);
        res.end(large_body);
    });

    start_server();

    H2TestPeer client(cli_fd_);
    int32_t id = client.get("/large");
    ASSERT_GT(id, 0);

    EXPECT_TRUE(client.wait_done(id, 5000)) << "timed out waiting for large response";

    auto* r = client.response(id);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->status, 200);
    EXPECT_FALSE(r->reset);
    EXPECT_EQ(r->body.size(), kLargeBodySize)
        << "partial delivery: received " << r->body.size()
        << " bytes, expected " << kLargeBodySize;
    EXPECT_EQ(r->body, large_body);
}
