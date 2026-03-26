// h2_server.hpp — Http2Server top-level orchestrator.
// Creates the listening socket, accepts connections, distributes them
// round-robin to EventLoop workers, and handles graceful shutdown.
//
// Per-worker connection ownership (Revision-3 C3):
//   No global connections_ vector or conn_mutex_.  Each EventLoop owns its
//   own unordered_map<int, shared_ptr<Connection>>; all mutation is
//   thread-local.
//
// Shutdown snapshot iteration (Revision-3 C4):
//   stop() signals the accept loop via eventfd.  graceful_shutdown() posts
//   per-worker lambdas that take a SNAPSHOT COPY of each worker's
//   connections map before iterating, so close() callbacks can safely
//   modify the live map during iteration.
#pragma once
#include "h2_constants.hpp"
#include "h2_router.hpp"
#include "h2_thread_pool.hpp"
#include <nghttp2/nghttp2.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace h2 {

class Connection;  // forward declaration — complete type in h2_connection.hpp

class Http2Server {
 public:
  Http2Server();
  ~Http2Server();

  Http2Server(const Http2Server&)            = delete;
  Http2Server& operator=(const Http2Server&) = delete;
  Http2Server(Http2Server&&)                 = delete;
  Http2Server& operator=(Http2Server&&)      = delete;

  // Set number of worker threads.  Must be called before listen_and_serve().
  // If not called (or set to 0), defaults to hardware_concurrency (min 1).
  void num_threads(size_t n);

  // Register a route handler.  Delegates to Router::handle().
  // Must be called before listen_and_serve().
  void handle(const std::string& pattern, RequestHandler handler);

  // Bind socket, start worker pool, and accept connections.
  // Blocks until stop() is called, then performs graceful shutdown.
  // Returns false (and sets error_msg) on socket/bind/listen failure.
  bool listen_and_serve(std::string& error_msg,
                        const std::string& addr,
                        const std::string& port);

  // Signal graceful shutdown: breaks accept_loop(), triggers GOAWAY on all
  // connections.  Thread-safe; safe to call from any thread.
  void stop();

 private:
  // Verify nghttp2 runtime version == v1.68.0 (0x014400).
  // Logs the version string at startup (Revision-4 R3).
  // Throws std::runtime_error on mismatch.
  void verify_nghttp2_version();

  // Level-triggered epoll loop on listen_fd_ + shutdown_event_fd_.
  // Runs on the caller thread of listen_and_serve().
  // Returns when shutdown_event_fd_ fires or running_ becomes false.
  void accept_loop();

  // Post per-worker shutdown lambdas (snapshot GOAWAY + drain timer).
  // Called after accept_loop() returns; blocks semantically until
  // pool_->stop() joins all worker threads.
  void graceful_shutdown();

  // ── Route table ───────────────────────────────────────────────────────
  Router router_;

  // ── Worker pool ───────────────────────────────────────────────────────
  std::unique_ptr<ThreadPool> pool_;
  size_t num_threads_ = 0;  // 0 → ThreadPool defaults to hardware_concurrency

  // ── Listen socket ─────────────────────────────────────────────────────
  int listen_fd_ = -1;

  // ── Shutdown coordination ────────────────────────────────────────────
  std::atomic<bool>       running_{false};
  int                     shutdown_event_fd_ = -1;  // eventfd

  std::mutex              stop_mutex_;
  std::condition_variable stop_cv_;
  bool                    stopped_ = false;
};

}  // namespace h2
