/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_HTTP2_SERVER_SEEN
#define FILE_HTTP2_SERVER_SEEN

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nghttp2/nghttp2.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include "thread-pool.h"

typedef struct ssl_ctx_st SSL_CTX;

// Forward declarations
class http2_server;
struct http2_connection;
struct http2_stream;
struct thread_pool_work_item;

// Response Body Provider
struct response_body;

// Public Request Type
struct http2_request {
  std::string method;  // GET, PUT, POST, DELETE, PATCH
  std::string path;    // path component only (before '?')
  std::string
      raw_query;  // query string portion of path (after '?', no leading '?')
  std::string scheme;     // scheme
  std::string authority;  // authority
  std::map<std::string, std::string>
      headers;       // regular headers (all lowercase)
  std::string body;  // accumulated request body (all DATA chunks concatenated)
  int32_t stream_id = 0;
};

// Public Response Type
class http2_response {
 public:
  // Write response status + headers + body to the stream.
  // Calls nghttp2_submit_response() and nghttp2_session_send() internally —
  // HPACK encoding completes while all header string data is still on the
  // stack.  Must be called exactly once per request.
  void send(
      int status_code, const std::map<std::string, std::string>& headers,
      const std::string& body);

  // Variant: send with no body (e.g. 204 No Content)
  void send(int status_code, const std::map<std::string, std::string>& headers);

  // Returns true if send() has already been called on this response.
  bool was_sent() const;

  // Synchronous constructor (event-loop-thread use — session/bev must remain
  // alive for the duration of send()).
  http2_response(
      nghttp2_session* session, int32_t stream_id, struct bufferevent* bev,
      http2_stream* stream, http2_server* server = nullptr);

  // Threaded constructor — response data is captured into the work item and
  // submitted to nghttp2 on the event loop thread via response_post_cb.
  explicit http2_response(thread_pool_work_item* work_item);

 private:
  nghttp2_session* session_         = nullptr;
  int32_t stream_id_                = 0;
  struct bufferevent* bev_          = nullptr;
  http2_stream* stream_             = nullptr;  // for response_body ownership
  http2_server* server_             = nullptr;
  bool sent_                        = false;    // guard against double-send
  thread_pool_work_item* work_item_ = nullptr;  // non-null in threaded mode
};

// Handler callback type: receives a fully accumulated request + response writer
using http2_handler =
    std::function<void(const http2_request&, http2_response&)>;

constexpr size_t HTTP2_LATENCY_BUCKET_COUNT = 12;

struct http2_latency_histogram_snapshot {
  std::array<uint64_t, HTTP2_LATENCY_BUCKET_COUNT> buckets{};
};

struct http2_server_metrics_snapshot {
  uint64_t total_requests_treated        = 0;
  uint64_t total_requests_completed      = 0;
  uint64_t total_streams_opened          = 0;
  uint64_t active_streams                = 0;
  uint64_t active_connections            = 0;
  uint64_t rejected_connections          = 0;
  uint64_t active_route_handlers         = 0;
  uint64_t max_active_route_handlers     = 0;
  uint64_t worker_enqueue_rejections     = 0;
  uint64_t request_body_limit_rejections = 0;
  uint64_t stream_resets                 = 0;
  uint64_t goaway_submitted              = 0;
  uint64_t response_submit_failures      = 0;
  uint64_t event_loop_post_failures      = 0;
  uint64_t tls_handshakes_started        = 0;
  uint64_t tls_handshakes_succeeded      = 0;
  uint64_t tls_handshakes_failed         = 0;
  uint64_t tls_alpn_h2_selected          = 0;
  uint64_t tls_alpn_missing_or_rejected  = 0;
  uint64_t status_1xx                    = 0;
  uint64_t status_2xx                    = 0;
  uint64_t status_3xx                    = 0;
  uint64_t status_4xx                    = 0;
  uint64_t status_5xx                    = 0;
  uint64_t status_other                  = 0;
  size_t worker_queue_depth              = 0;
  size_t worker_active_tasks             = 0;
  size_t worker_queue_capacity           = 0;
  http2_latency_histogram_snapshot request_total_us;
  http2_latency_histogram_snapshot queue_wait_us;
  http2_latency_histogram_snapshot handler_duration_us;
  http2_latency_histogram_snapshot postback_delay_us;
};

// Server Configuration
struct http2_server_config {
  // SETTINGS frame values sent to clients
  uint32_t max_concurrent_streams = 1000;
  uint32_t initial_window_size    = 65535;  // per-stream flow control window
  uint32_t max_header_list_size =
      65536;  // HPACK bomb protection (SETTINGS frame)

  // Application-level limits
  size_t max_request_body_size = 1 * 1024 * 1024;  // 1 MB
  uint32_t max_connections     = 10000;

  // Timeouts (seconds)
  int connection_idle_timeout_sec = 60;
  int shutdown_drain_timeout_sec  = 5;
  int listener_backlog            = -1;

  // Thread pool (0 = synchronous/event-loop-only mode)
  uint32_t num_worker_threads = 4;
  size_t max_pending_tasks    = 10000;

  // TLS/HTTPS. Disabled by default to preserve existing h2c behavior.
  bool enable_tls = false;
  std::string tls_cert_chain_path;
  std::string tls_private_key_path;
  std::string tls_ca_path;

  // Mutual TLS: when true, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT
  // is enforced. Requires tls_ca_path to be set. Disabled by default.
  bool enable_mtls = false;

  // Periodic stats log interval in seconds (0 = disabled).
  // When non-zero, a recurring timer fires on the event-loop thread every
  // stats_log_interval_sec seconds and logs a counter snapshot identical to
  // the shutdown summary.  Counters are read via metrics_snapshot() which
  // accesses atomics — no additional locking needed.
  uint32_t stats_log_interval_sec = 0;
};

struct http2_server_metrics {
  std::atomic<uint64_t> total_requests_treated{0};
  std::atomic<uint64_t> total_requests_completed{0};
  std::atomic<uint64_t> total_streams_opened{0};
  std::atomic<uint64_t> active_streams{0};
  std::atomic<uint64_t> active_connections{0};
  std::atomic<uint64_t> rejected_connections{0};
  std::atomic<uint64_t> active_route_handlers{0};
  std::atomic<uint64_t> max_active_route_handlers{0};
  std::atomic<uint64_t> worker_enqueue_rejections{0};
  std::atomic<uint64_t> request_body_limit_rejections{0};
  std::atomic<uint64_t> stream_resets{0};
  std::atomic<uint64_t> goaway_submitted{0};
  std::atomic<uint64_t> response_submit_failures{0};
  std::atomic<uint64_t> event_loop_post_failures{0};
  std::atomic<uint64_t> tls_handshakes_started{0};
  std::atomic<uint64_t> tls_handshakes_succeeded{0};
  std::atomic<uint64_t> tls_handshakes_failed{0};
  std::atomic<uint64_t> tls_alpn_h2_selected{0};
  std::atomic<uint64_t> tls_alpn_missing_or_rejected{0};
  std::atomic<uint64_t> status_1xx{0};
  std::atomic<uint64_t> status_2xx{0};
  std::atomic<uint64_t> status_3xx{0};
  std::atomic<uint64_t> status_4xx{0};
  std::atomic<uint64_t> status_5xx{0};
  std::atomic<uint64_t> status_other{0};
  std::array<std::atomic<uint64_t>, HTTP2_LATENCY_BUCKET_COUNT>
      request_total_us{};
  std::array<std::atomic<uint64_t>, HTTP2_LATENCY_BUCKET_COUNT> queue_wait_us{};
  std::array<std::atomic<uint64_t>, HTTP2_LATENCY_BUCKET_COUNT>
      handler_duration_us{};
  std::array<std::atomic<uint64_t>, HTTP2_LATENCY_BUCKET_COUNT>
      postback_delay_us{};
};

// Server Class

class http2_server {
 public:
  http2_server(
      const std::string& address, uint32_t port,
      http2_server_config config = {});
  ~http2_server();

  // Non-copyable, non-movable
  http2_server(const http2_server&) = delete;
  http2_server& operator=(const http2_server&) = delete;

  // Register a route handler matched by longest-prefix.
  // Semantics identical to nghttp2-asio server.handle().
  void handle(const std::string& path_prefix, http2_handler handler);

  // Start the server. Blocks until stop() is called from another thread
  // (runs the libevent event loop internally). Returns false if startup fails
  // before the listener is active.
  //
  // on_ready (optional): if non-null, called with true once the listener socket
  // is bound and the server is accepting connections, or with false if startup
  // fails before event_base_dispatch() is entered.  The callback fires on the
  // calling thread (before event_base_dispatch blocks), so callers can use a
  // std::promise to get readiness notification without polling.
  bool start(std::function<void(bool)> on_ready = nullptr);

  // Graceful shutdown — thread-safe, may be called from any thread.
  // Flow: stop() → event_base_once(goaway_and_drain_cb)
  //             → drain_timer_cb → event_base_loopbreak()
  void stop();

  // Config accessors.
  const http2_server_config& config() const { return config_; }
  http2_server_config& config() { return config_; }

  // Route lookup — public so file-scope nghttp2 callbacks
  // (on_frame_recv_callback etc.) in the .cpp can call
  // conn->server->find_handler() without friendship.
  http2_handler* find_handler(const std::string& path);

  // Thread pool accessors (called from event-loop callbacks).
  bool has_thread_pool() const { return pool_ != nullptr; }
  thread_pool* get_thread_pool() { return pool_.get(); }
  bool is_shutting_down() const { return shutting_down_; }
  bool is_running() const { return running_.load(std::memory_order_acquire); }
  struct event_base* base() const {
    return base_;
  }
  http2_server_metrics_snapshot metrics_snapshot() const;

  void record_request_treated();
  void record_response_completed(int status_code);
  uint64_t record_response_submit_failure();
  uint64_t record_event_loop_post_failure();
  uint64_t record_worker_enqueue_rejection();
  void record_request_body_limit_rejection();
  void record_stream_reset();
  void record_goaway_submitted();
  void record_stream_opened();
  void record_stream_closed();
  uint64_t record_connection_rejected();
  void record_tls_handshake_started();
  void record_tls_handshake_succeeded();
  uint64_t record_tls_handshake_failed();
  void record_tls_alpn_h2_selected();
  uint64_t record_tls_alpn_missing_or_rejected();
  void record_route_handler_started();
  void record_route_handler_completed(uint64_t duration_us);
  void record_request_total(uint64_t duration_us);
  void record_queue_wait(uint64_t duration_us);
  void record_postback_delay(uint64_t duration_us);

  // Static callback registered via event_base_once() by worker threads.
  // Must be public so lambdas in file-scope nghttp2 callbacks can reference it.
  static void response_post_cb(evutil_socket_t fd, short what, void* arg);

  // Safety-timeout callback scheduled by start_deferred_destruction().
  // Must be public so http2_connection::start_deferred_destruction() can
  // pass it as a C function pointer to event_base_once().
  static void deferred_destruction_timeout_cb(
      evutil_socket_t fd, short what, void* arg);

 private:
  // libevent primitives
  struct event_base* base_         = nullptr;
  struct evconnlistener* listener_ = nullptr;
  struct event* drain_timer_       = nullptr;
  SSL_CTX* ssl_ctx_                = nullptr;

  // server state
  std::string address_;
  uint32_t port_;
  http2_server_config config_;
  std::atomic<bool> running_{false};

  // routing table (sorted longest-prefix-first after start())
  struct Route {
    std::string prefix;
    http2_handler handler;
  };
  std::vector<Route> routes_;

  // active connections (for GOAWAY / graceful shutdown)
  std::mutex connections_mutex_;
  std::unordered_map<uint64_t, http2_connection*> connections_;

  // Internal libevent callbacks (static for C-function-pointer compatibility).
  // NOTE: `static` appears ONLY in these in-class declarations.
  // The out-of-class definitions in http2-server.cpp must NOT use `static`.
  static void accept_cb(
      struct evconnlistener* listener, evutil_socket_t fd,
      struct sockaddr* addr, int addrlen, void* arg);
  static void read_cb(struct bufferevent* bev, void* arg);
  static void event_cb(struct bufferevent* bev, short events, void* arg);
  static void goaway_and_drain_cb(evutil_socket_t fd, short what, void* arg);
  static void drain_timer_cb(evutil_socket_t fd, short what, void* arg);
  static void stats_log_timer_cb(evutil_socket_t fd, short what, void* arg);

  // Internal helpers
  void add_connection(http2_connection* conn);
  void remove_connection(http2_connection* conn);
  void close_all_connections();
  http2_connection* find_connection(uint64_t conn_id);

  // Thread pool state (only valid when pool_ != nullptr)
  std::unique_ptr<thread_pool> pool_;
  bool shutting_down_ = false;  // event-loop thread only
  std::atomic<uint64_t> next_conn_id_{1};
  http2_server_metrics metrics_;

  bool init_tls_context();
  void free_tls_context();
};

#endif  // FILE_HTTP2_SERVER_SEEN
