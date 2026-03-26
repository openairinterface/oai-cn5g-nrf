// h2_server.cpp — Http2Server implementation.
// See h2_server.hpp for class documentation and design rationale.
#include "h2_server.hpp"
#include "h2_connection.hpp"  // complete Connection type + nghttp2 headers

#include <netdb.h>            // getaddrinfo, freeaddrinfo, addrinfo, AI_PASSIVE
#include <sys/epoll.h>        // epoll_create1, epoll_ctl, epoll_wait
#include <sys/eventfd.h>      // eventfd, EFD_NONBLOCK, EFD_CLOEXEC
#include <sys/socket.h>       // socket, bind, listen, accept4, setsockopt
#include <unistd.h>           // close, write

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace h2 {

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

Http2Server::Http2Server() = default;

Http2Server::~Http2Server() {
  // If listen_and_serve() is still blocking (e.g. process receives SIGTERM
  // before stop() is called), ensure we signal it.
  if (running_.load(std::memory_order_relaxed)) {
    stop();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Configuration setters (must be called before listen_and_serve)
// ─────────────────────────────────────────────────────────────────────────────

void Http2Server::num_threads(size_t n) {
  num_threads_ = n;
}

void Http2Server::handle(const std::string& pattern, RequestHandler handler) {
  (void)router_.handle(pattern, std::move(handler));
}

// ─────────────────────────────────────────────────────────────────────────────
// verify_nghttp2_version
// ─────────────────────────────────────────────────────────────────────────────

void Http2Server::verify_nghttp2_version() {
  const nghttp2_info* info = nghttp2_version(0);

  // Log version at startup (Revision-4 R3).
  char hex_buf[16];
  std::snprintf(hex_buf, sizeof(hex_buf), "0x%06x", info->version_num);
  std::fprintf(stderr,
               "[h2::Http2Server] nghttp2 runtime version: %s (%s)\n",
               info->version_str, hex_buf);

  // Exact pin: v1.68.0 → NGHTTP2_VERSION_NUM == 0x014400
  // (Note: plan mistakenly states 0x016800; correct encoding verified in Task 0.0)
  if (info->version_num != 0x014400) {
    char want_buf[16];
    std::snprintf(want_buf, sizeof(want_buf), "0x%06x", 0x014400);
    throw std::runtime_error(
        std::string("nghttp2 version mismatch: got ") + info->version_str +
        " (" + hex_buf + "), need exactly v1.68.0 (" + want_buf + ")");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// listen_and_serve  (public, blocking)
// ─────────────────────────────────────────────────────────────────────────────

bool Http2Server::listen_and_serve(std::string& error_msg,
                                   const std::string& addr,
                                   const std::string& port) {
  // Reentrancy guard: only one call may proceed at a time.
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    error_msg = "already running";
    return false;
  }

  // Runtime version check + startup log (throws on mismatch).
  verify_nghttp2_version();

  // ── Resolve address ──────────────────────────────────────────────────
  struct addrinfo hints{};
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags    = AI_PASSIVE;

  // Pass nullptr for addr when empty so getaddrinfo binds to 0.0.0.0.
  const char* node = addr.empty() ? nullptr : addr.c_str();
  struct addrinfo* res = nullptr;
  if (::getaddrinfo(node, port.c_str(), &hints, &res) != 0) {
    error_msg = "getaddrinfo: " + std::string(std::strerror(errno));
    running_ = false;
    return false;
  }

  // ── Create non-blocking listen socket ────────────────────────────────
  listen_fd_ = ::socket(res->ai_family,
                        res->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                        res->ai_protocol);
  if (listen_fd_ < 0) {
    ::freeaddrinfo(res);
    error_msg = "socket: " + std::string(std::strerror(errno));
    running_ = false;
    return false;
  }

  int on = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));

  if (::bind(listen_fd_, res->ai_addr, res->ai_addrlen) < 0) {
    ::freeaddrinfo(res);
    ::close(listen_fd_);
    listen_fd_ = -1;
    error_msg = "bind: " + std::string(std::strerror(errno));
    running_ = false;
    return false;
  }
  ::freeaddrinfo(res);
  res = nullptr;

  // kListenBacklog = 128 (explicit constant, avoids SOMAXCONN dependency).
  if (::listen(listen_fd_, kListenBacklog) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    error_msg = "listen: " + std::string(std::strerror(errno));
    running_ = false;
    return false;
  }

  // ── Shutdown eventfd ─────────────────────────────────────────────────
  shutdown_event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (shutdown_event_fd_ < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    error_msg = "eventfd: " + std::string(std::strerror(errno));
    running_ = false;
    return false;
  }

  // ── Start worker pool ─────────────────────────────────────────────────
  pool_ = std::make_unique<ThreadPool>(num_threads_);
  pool_->start();

  // ── Accept loop (blocks until stop() fires shutdown_event_fd_) ───────
  accept_loop();

  // ── Graceful shutdown ─────────────────────────────────────────────────
  graceful_shutdown();

  // Join all worker threads (they exit when their loops stop).
  pool_->stop();

  ::close(listen_fd_);
  listen_fd_ = -1;
  {
    std::lock_guard<std::mutex> lk(stop_mutex_);
    ::close(shutdown_event_fd_);
    shutdown_event_fd_ = -1;
    stopped_ = true;
  }
  stop_cv_.notify_all();
  running_ = false;

  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// accept_loop  (private, level-triggered epoll on listen + shutdown fds)
// ─────────────────────────────────────────────────────────────────────────────

void Http2Server::accept_loop() {
  int epfd = ::epoll_create1(EPOLL_CLOEXEC);
  if (epfd < 0) {
    std::fprintf(stderr, "[h2::Http2Server] epoll_create1: %s\n",
                 std::strerror(errno));
    running_ = false;
    return;
  }

  struct epoll_event ev{};
  ev.events  = EPOLLIN;
  ev.data.fd = listen_fd_;
  ::epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd_, &ev);

  ev.events  = EPOLLIN;
  ev.data.fd = shutdown_event_fd_;
  ::epoll_ctl(epfd, EPOLL_CTL_ADD, shutdown_event_fd_, &ev);

  struct epoll_event events[16];

  while (running_) {
    int nfds = ::epoll_wait(epfd, events, 16, -1);
    if (nfds < 0) {
      if (errno == EINTR) continue;  // signal delivery — loop again
      std::fprintf(stderr, "[h2::Http2Server] epoll_wait: %s\n",
                   std::strerror(errno));
      break;
    }

    for (int i = 0; i < nfds; ++i) {
      if (events[i].data.fd == shutdown_event_fd_) {
        running_ = false;
        break;
      }

      if (events[i].data.fd != listen_fd_) continue;

      // Level-triggered: drain all pending connections in one pass.
      // The outer epoll_wait loop will not re-fire until we return.
      while (true) {
        int client_fd = ::accept4(listen_fd_, nullptr, nullptr,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // queue empty
          if (errno == EINTR) continue;
          std::fprintf(stderr, "[h2::Http2Server] accept4: %s\n",
                       std::strerror(errno));
          break;
        }

        // Round-robin dispatch to a worker EventLoop.
        EventLoop* lp = &pool_->get_next_loop();

        // Post to the owning worker — Connection state lives ONLY on that
        // thread.  Capture lp as a pointer (not [&loop]) per Revision-4 C4:
        // [&loop] would be a dangling reference if the deferred lambda
        // executes after this scope or a future iteration moves the reference.
        lp->post([this, lp, client_fd]() {
          // Connection limit: reject if this worker is at capacity.
          if (lp->connections().size() >= kMaxConnectionsPerWorker) {
            ::close(client_fd);
            return;
          }
          // Create Connection on the owning thread so all state is thread-local
          // from the first moment — no cross-thread sharing ever occurs.
          auto conn = std::make_shared<Connection>(client_fd, *lp, router_);
          lp->add_connection(client_fd, conn);
          if (conn->start() != 0) {
            conn->close();
          }
        });
      }
    }
  }

  ::close(epfd);
}

// ─────────────────────────────────────────────────────────────────────────────
// graceful_shutdown  (private)
// ─────────────────────────────────────────────────────────────────────────────
//
// Per-worker snapshot iteration (Revision-3 C4 / discovery §10.1):
//   Phase 1 — shutdown notice (SETTINGS_MAX_CONCURRENT_STREAMS=0)
//   Phase 2 — final GOAWAY after kShutdownNoticeDelayMs
//   Phase 3 — force-close after kDrainTimeoutMs
//
// All work is posted to each worker's own EventLoop; no cross-thread
// iteration, no shared mutex.  Each phase copies the connections map before
// iterating so that close callbacks (which remove entries from the live map)
// cannot invalidate iterators.

void Http2Server::graceful_shutdown() {
  for (auto& loop_ptr : pool_->loops()) {
    EventLoop* lp = loop_ptr.get();

    lp->post([lp]() {
      lp->set_shutting_down(true);

      // ── Phase 1: Shutdown notices ───────────────────────────────────
      // SNAPSHOT: flush_output() may trigger a write error → close() →
      // remove_connection().  Iterate a copy; live map may shrink.
      auto notice_snapshot = lp->connections();
      for (auto& [fd, conn] : notice_snapshot) {
        (void)fd;
        if (conn->session()) {
          nghttp2_submit_shutdown_notice(conn->session());
          conn->flush_output();
          conn->set_shutdown_state(Connection::ShutdownState::SHUTDOWN_NOTICE_SENT);
        }
      }

      // If all connections already closed themselves, stop immediately.
      if (lp->connections().empty()) {
        lp->stop();
        return;
      }

      // ── Phase 2: Final GOAWAY after kShutdownNoticeDelayMs ──────────
      lp->add_timer(static_cast<uint64_t>(kShutdownNoticeDelayMs),
                    [lp](uint32_t /*events*/) {
        // SNAPSHOT: begin_draining() may call close().
        auto goaway_snapshot = lp->connections();
        for (auto& [fd, conn] : goaway_snapshot) {
          (void)fd;
          if (conn->shutdown_state() !=
              Connection::ShutdownState::SHUTDOWN_NOTICE_SENT) {
            continue;
          }
          if (conn->session()) {
            int32_t last_id =
                nghttp2_session_get_last_proc_stream_id(conn->session());
            nghttp2_submit_goaway(conn->session(), NGHTTP2_FLAG_NONE,
                                  last_id, NGHTTP2_NO_ERROR, nullptr, 0);
            conn->flush_output();
            conn->set_shutdown_state(
                Connection::ShutdownState::FINAL_GOAWAY_SENT);
          }
          // Enter drain mode: Connection self-closes when active streams == 0.
          conn->begin_draining();
        }

        if (lp->connections().empty()) {
          lp->stop();
          return;
        }

        // ── Phase 3: Force-close after kDrainTimeoutMs ──────────────
        lp->add_timer(static_cast<uint64_t>(kDrainTimeoutMs),
                      [lp](uint32_t /*events*/) {
          // SNAPSHOT: force_close() calls close() → modifies live map.
          auto remaining = lp->connections();
          for (auto& [fd, conn] : remaining) {
            (void)fd;
            if (conn->shutdown_state() != Connection::ShutdownState::CLOSED) {
              conn->force_close();
            }
          }
          lp->stop();
        });
      });
    });
  }
  // pool_->stop() (called by listen_and_serve() after this function returns)
  // joins all worker threads.  Workers exit when EventLoop::run() returns,
  // which happens after lp->stop() fires (from Phase 2 empty-check or
  // Phase 3 timeout).
}

// ─────────────────────────────────────────────────────────────────────────────
// stop  (public, thread-safe)
// ─────────────────────────────────────────────────────────────────────────────

void Http2Server::stop() {
  running_ = false;
  {
    std::lock_guard<std::mutex> lk(stop_mutex_);
    if (shutdown_event_fd_ >= 0) {
      const uint64_t val = 1;
      (void)::write(shutdown_event_fd_, &val, sizeof(val));
    }
  }
}

}  // namespace h2
