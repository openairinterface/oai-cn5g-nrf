// h2_connection.hpp
// Manages a single HTTP/2 TCP connection: owns the socket fd, nghttp2_session*,
// read/write buffers, per-stream state, and idle timer.
// Pinned to one EventLoop thread — no mutex needed on any per-connection state.
#pragma once
#include "h2_constants.hpp"
#include "h2_event_loop.hpp"

#include <nghttp2/nghttp2.h>
#include <sys/types.h>   // ssize_t

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace h2 {

class Router;
class Request;
class Response;

// Per-stream state (replaces nghttp2-asio's `stream` class).
// One StreamData is created for each incoming HTTP/2 request stream and lives
// until on_stream_close_cb fires.
struct StreamData {
  int32_t stream_id      = 0;
  std::string method;
  std::string path;
  std::string raw_query;
  std::string scheme;
  std::string authority;
  std::multimap<std::string, std::string> headers;
  std::string body;
  size_t header_buffer_size   = 0;    // accrued decompressed header bytes
  bool headers_complete       = false;
  // Set when accumulated body would exceed kMaxRequestBodySize.  Once true,
  // further DATA chunks are dropped (window still consumed to avoid stall)
  // and on_frame_recv_cb will RST_STREAM instead of dispatching.
  bool body_limit_exceeded    = false;
};

// Connection wraps a single accepted TCP socket and its nghttp2 server session.
// All public methods must be called from the owning EventLoop thread.
class Connection : public std::enable_shared_from_this<Connection> {
 public:
  // fd: accepted client socket (caller-owned until start() succeeds).
  // loop: the EventLoop this connection is pinned to.
  // router: shared (read-only after setup) route table.
  Connection(int fd, EventLoop& loop, Router& router);
  ~Connection();

  Connection(const Connection&)            = delete;
  Connection& operator=(const Connection&) = delete;

  // Initialize nghttp2 session, set socket non-blocking, register with epoll,
  // send initial SETTINGS frame.
  // Returns 0 on success, -1 on error (does NOT close fd on error).
  int start();

  // Graceful connection teardown:
  //   1. GOAWAY (RFC 9113 §6.8)
  //   2. Best-effort flush of pending writes
  //   3. Cancel idle timer
  //   4. Remove fd from epoll
  //   5. nghttp2_session_del
  //   6. ::close(fd_)
  //   7. Clear stream / response-body state
  //   8. Remove self from owning EventLoop's connection map
  // Safe to call multiple times (guarded by closed_).
  void close();

  // Dispatch epoll events to do_read() / do_write().
  void on_epoll_event(uint32_t events);

  // Queue an HTTP/2 response for stream_id. Must be called from owning thread.
  void submit_response(int32_t stream_id, uint16_t status_code,
                       const std::multimap<std::string, std::string>& headers,
                       std::string body);

  // Called by data_provider_read_cb to deliver response body bytes to nghttp2.
  ssize_t read_response_data(int32_t stream_id, uint8_t* buf, size_t length,
                             uint32_t* data_flags);

  int fd() const { return fd_; }

  // The EventLoop this connection is pinned to (same thread as all ops).
  EventLoop& owning_loop() { return loop_; }

  enum class ShutdownState {
    RUNNING,
    SHUTDOWN_NOTICE_SENT,
    FINAL_GOAWAY_SENT,
    DRAINING,
    FORCE_CLOSE,
    CLOSED,
  };

  ShutdownState shutdown_state() const { return shutdown_state_; }
  void set_shutdown_state(ShutdownState s) { shutdown_state_ = s; }

  // Enter drain mode: stop accepting new streams; close when active_streams_ == 0.
  void begin_draining();

  // Hard close: terminate session immediately.
  void force_close();

  // Best-effort flush of pending write data (public wrapper for shutdown).
  void flush_output();

  nghttp2_session* session() const { return session_; }

 private:
  // ---- nghttp2 session callbacks (static, forwarded via user_data) ----

  static int on_begin_headers_cb(nghttp2_session* session,
                                 const nghttp2_frame* frame, void* user_data);
  static int on_header_cb(nghttp2_session* session, const nghttp2_frame* frame,
                          const uint8_t* name, size_t namelen,
                          const uint8_t* value, size_t valuelen,
                          uint8_t flags, void* user_data);
  static int on_frame_recv_cb(nghttp2_session* session,
                              const nghttp2_frame* frame, void* user_data);
  static int on_data_chunk_recv_cb(nghttp2_session* session, uint8_t flags,
                                   int32_t stream_id, const uint8_t* data,
                                   size_t len, void* user_data);
  static int on_stream_close_cb(nghttp2_session* session, int32_t stream_id,
                                uint32_t error_code, void* user_data);
  static int on_frame_send_cb(nghttp2_session* session,
                              const nghttp2_frame* frame, void* user_data);
  static int on_frame_not_send_cb(nghttp2_session* session,
                                  const nghttp2_frame* frame,
                                  int lib_error_code, void* user_data);

  // Static trampoline for nghttp2_data_provider read callback.
  static ssize_t data_provider_read_cb(nghttp2_session* session,
                                       int32_t stream_id, uint8_t* buf,
                                       size_t length, uint32_t* data_flags,
                                       nghttp2_data_source* source,
                                       void* user_data);

  // ---- Internal I/O methods ----

  void do_read();
  void do_write();

  // Populate write_buf_ from nghttp2_session_mem_send(). Returns 0 on success.
  // Resets write_buf_len_ / write_buf_offset_ — call only after current buffer
  // has been fully drained to the socket.
  int  fill_write_buf();

  // Best-effort blocking flush: fill then write. Used during close().
  void flush_write_buf();

  // Defer a do_write() call onto the owning EventLoop (idempotent).
  void signal_write();

  // Re-arm the idle timerfd to kIdleTimeoutMs from now.
  void reset_idle_timer();

  // ---- Stream management ----

  StreamData* create_stream(int32_t stream_id);
  StreamData* find_stream(int32_t stream_id);
  void        close_stream(int32_t stream_id);
  void        dispatch_request(StreamData& strm);

  // ---- State ----

  int       fd_;
  EventLoop& loop_;
  Router&   router_;
  nghttp2_session* session_ = nullptr;

  // 8 KB read buffer — data read from socket before passing to nghttp2.
  std::array<uint8_t, kReadBufSize>  read_buf_;

  // 64 KB write buffer — data COPIED from nghttp2_session_mem_send() before
  // socket write. The pointer from mem_send() is invalid after the next call,
  // so we copy immediately.
  std::array<uint8_t, kWriteBufSize> write_buf_;
  size_t write_buf_len_    = 0;
  size_t write_buf_offset_ = 0;

  // Holds a pointer returned by mem_send() that didn't fit in write_buf_.
  // Valid until the next nghttp2_session_mem_send() call.
  const uint8_t* overflow_buf_    = nullptr;
  size_t         overflow_buflen_ = 0;

  std::unordered_map<int32_t, StreamData> streams_;
  int active_streams_ = 0;

  // Response body storage indexed by stream_id.
  std::unordered_map<int32_t, std::string> response_bodies_;
  std::unordered_map<int32_t, size_t>      response_body_offsets_;

  // Write-readiness state.
  bool write_pending_  = false;  // EPOLLOUT registered, waiting for socket
  bool write_signaled_ = false;  // do_write() already posted via loop_.post()

  int  idle_timer_fd_ = -1;
  bool closed_        = false;
  ShutdownState shutdown_state_ = ShutdownState::RUNNING;
};

}  // namespace h2
