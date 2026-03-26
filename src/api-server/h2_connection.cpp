// h2_connection.cpp
#include "h2_connection.hpp"
#include "h2_request.hpp"    // complete Request type (constructor body in h2_request.cpp)
#include "h2_response.hpp"   // complete Response type (method bodies in h2_response.cpp)
#include "h2_router.hpp"     // complete Router type + RequestHandler

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string_view>
#include <vector>

namespace h2 {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

Connection::Connection(int fd, EventLoop& loop, Router& router)
    : fd_(fd), loop_(loop), router_(router) {}

Connection::~Connection() {
  // Hard-close fallback — runs only when close() was not called normally.
  if (!closed_) {
    if (session_) {
      nghttp2_session_del(session_);
      session_ = nullptr;
    }
    if (idle_timer_fd_ >= 0) {
      // cancel_timer calls remove_fd + close; call directly to avoid accessing
      // EventLoop which may be partially destroyed in exceptional paths.
      ::close(idle_timer_fd_);
      idle_timer_fd_ = -1;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }
}

// ---------------------------------------------------------------------------
// start()
// ---------------------------------------------------------------------------

int Connection::start() {
  // ---- Create nghttp2 server session ----
  nghttp2_session_callbacks* callbacks;
  if (nghttp2_session_callbacks_new(&callbacks) != 0) return -1;

  // RAII cleanup of callbacks object.
  struct CbDeleter {
    void operator()(nghttp2_session_callbacks* p) const {
      nghttp2_session_callbacks_del(p);
    }
  };
  std::unique_ptr<nghttp2_session_callbacks, CbDeleter> cb_guard(callbacks);

  nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_cb);
  nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_cb);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_cb);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_cb);
  nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_cb);
  nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, on_frame_send_cb);
  nghttp2_session_callbacks_set_on_frame_not_send_callback(callbacks, on_frame_not_send_cb);

  nghttp2_option* option;
  nghttp2_option_new(&option);
  // Manual flow control: we update window ourselves so we can apply back-pressure.
  nghttp2_option_set_no_auto_window_update(option, 1);
  // Flood detection defaults are kept:
  //   max_outbound_ack: 1000  (SETTINGS/PING ACK flood)
  //   stream_reset_rate_limit: burst=1000, rate=33  (RST_STREAM flood)
  //   max_continuations: 8  (CONTINUATION flood)

  int rv = nghttp2_session_server_new2(&session_, callbacks, this, option);
  nghttp2_option_del(option);
  if (rv != 0) return -1;

  // ---- Submit SETTINGS frame ----
  nghttp2_settings_entry settings[] = {
    {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, kMaxConcurrentStreams},
    {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,    kInitialWindowSize},
    {NGHTTP2_SETTINGS_MAX_FRAME_SIZE,         kMaxFrameSize},
    {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE,   static_cast<uint32_t>(kMaxHeaderListSize)},
  };
  nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE,
                          settings, sizeof(settings) / sizeof(settings[0]));

  // ---- Set socket non-blocking ----
  int flags = fcntl(fd_, F_GETFL, 0);
  fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

  // ---- Register with epoll (level-triggered, no EPOLLET) ----
  // EPOLLRDHUP: detect peer half-close early (R2 fix).
  // Callbacks capture weak_from_this() so they do not prevent destruction if
  // the last owning shared_ptr is released before the fd/timer is removed.
  {
    auto weak_self = weak_from_this();
    loop_.add_fd(fd_, EPOLLIN | EPOLLRDHUP, [weak_self](uint32_t events) {
      if (auto conn = weak_self.lock()) conn->on_epoll_event(events);
    });

    // ---- Start idle timer ----
    idle_timer_fd_ = loop_.add_timer(kIdleTimeoutMs, [weak_self](uint32_t) {
      if (auto conn = weak_self.lock()) conn->close();
    }, /*oneshot=*/true);
  }

  // ---- Register ownership in EventLoop's connection map ----
  // The EventLoop's connections_ map holds the owning shared_ptr, keeping the
  // Connection alive for the duration of the session.  remove_connection() in
  // close() releases it.  Must be called after epoll registration so that any
  // event that fires immediately can still find a valid shared_ptr.
  loop_.add_connection(fd_, shared_from_this());

  // ---- Flush SETTINGS frame to socket ----
  do_write();
  return 0;
}

// ---------------------------------------------------------------------------
// close()
// ---------------------------------------------------------------------------

void Connection::close() {
  // C3 fix: hold a shared_ptr so removing self from the connections map cannot
  // destroy *this before close() returns.
  auto self = shared_from_this();

  if (closed_) return;
  closed_ = true;

  // Step 1: Submit GOAWAY (RFC 9113 §6.8) and best-effort flush.
  if (session_) {
    int32_t last_id = nghttp2_session_get_last_proc_stream_id(session_);
    nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE, last_id,
                          NGHTTP2_NO_ERROR, nullptr, 0);
    flush_write_buf();
  }

  // Step 2: Cancel idle timer.
  if (idle_timer_fd_ >= 0) {
    loop_.cancel_timer(idle_timer_fd_);
    idle_timer_fd_ = -1;
  }

  // Step 3: Remove socket from epoll.
  loop_.remove_fd(fd_);

  // Step 4: Destroy nghttp2 session.
  if (session_) {
    nghttp2_session_del(session_);
    session_ = nullptr;
  }

  // Step 5: Close socket.
  int saved_fd = fd_;
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }

  // Step 6: Clear all per-stream and response-body state.
  streams_.clear();
  response_bodies_.clear();
  response_body_offsets_.clear();

  // Step 7: Reset write buffer state.
  write_buf_len_    = 0;
  write_buf_offset_ = 0;
  overflow_buf_     = nullptr;
  overflow_buflen_  = 0;

  // Step 8: Remove self from owning EventLoop's connection map.
  // (Same thread — no lock needed.)
  shutdown_state_ = ShutdownState::CLOSED;
  if (saved_fd >= 0) {
    loop_.remove_connection(saved_fd);
  }

  // If this worker is shutting down and was the last connection, stop the loop.
  if (loop_.shutting_down() && loop_.connections().empty()) {
    loop_.stop();
  }
}

// ---------------------------------------------------------------------------
// Shutdown helpers
// ---------------------------------------------------------------------------

void Connection::begin_draining() {
  shutdown_state_ = ShutdownState::DRAINING;
  if (active_streams_ == 0) {
    close();
  }
  // Otherwise the loop keeps running; on_stream_close_cb will call close()
  // when the last in-flight stream completes.
}

void Connection::force_close() {
  shutdown_state_ = ShutdownState::FORCE_CLOSE;
  if (session_) {
    nghttp2_session_terminate_session(session_, NGHTTP2_NO_ERROR);
    flush_output();
  }
  close();
}

void Connection::flush_output() {
  // do_write() handles EAGAIN correctly (registers EPOLLOUT for retry),
  // so use it instead of flush_write_buf() which drops data on EAGAIN.
  do_write();
}

// ---------------------------------------------------------------------------
// I/O helpers
// ---------------------------------------------------------------------------

// fill_write_buf() — populate write_buf_ from nghttp2's outbound queue.
// MUST be called only after the previous write_buf_ contents have been fully
// drained to the socket (write_buf_offset_ == write_buf_len_ or initial state).
int Connection::fill_write_buf() {
  write_buf_len_    = 0;
  write_buf_offset_ = 0;

  // Handle data that was left over from a previous fill (didn't fit).
  // overflow_buf_ points into nghttp2's internal buffer and is valid until the
  // next nghttp2_session_mem_send() call — copy it first.
  if (overflow_buf_) {
    std::copy_n(overflow_buf_, overflow_buflen_, write_buf_.begin());
    write_buf_len_   = overflow_buflen_;
    overflow_buf_    = nullptr;
    overflow_buflen_ = 0;
  }

  for (;;) {
    const uint8_t* data;
    ssize_t nread = nghttp2_session_mem_send(session_, &data);
    if (nread < 0) return -1;
    if (nread == 0) break;

    size_t chunk = static_cast<size_t>(nread);
    if (write_buf_len_ + chunk > write_buf_.size()) {
      // Doesn't fit: save for next fill_write_buf() call.
      overflow_buf_    = data;
      overflow_buflen_ = chunk;
      break;
    }

    std::copy_n(data, chunk, write_buf_.begin() + write_buf_len_);
    write_buf_len_ += chunk;
  }
  return 0;
}

// flush_write_buf() — best-effort blocking write; used during close().
// Drains any residual write_buf_ data first, then fetches and writes
// any newly-queued nghttp2 frames (e.g. the GOAWAY submitted by close()).
void Connection::flush_write_buf() {
  // Phase A: drain any residual data already in write_buf_ (partial write
  // that was waiting for EPOLLOUT).  Do NOT call fill_write_buf() yet —
  // that would reset write_buf_ and lose the residual bytes.
  while (write_buf_offset_ < write_buf_len_) {
    ssize_t n = ::write(fd_, write_buf_.data() + write_buf_offset_,
                        write_buf_len_ - write_buf_offset_);
    if (n <= 0) break;
    write_buf_offset_ += static_cast<size_t>(n);
  }

  // Phase B: now that write_buf_ is drained (or we gave up on it),
  // fill with any newly-queued frames and do a best-effort write.
  fill_write_buf();
  while (write_buf_offset_ < write_buf_len_) {
    ssize_t n = ::write(fd_, write_buf_.data() + write_buf_offset_,
                        write_buf_len_ - write_buf_offset_);
    if (n <= 0) break;
    write_buf_offset_ += static_cast<size_t>(n);
  }
}

// do_read() — read from socket, feed bytes to nghttp2.
void Connection::do_read() {
  if (closed_) return;

  ssize_t nread = ::read(fd_, read_buf_.data(), read_buf_.size());
  if (nread < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    close();
    return;
  }
  if (nread == 0) {
    // EOF — peer closed the connection.
    close();
    return;
  }

  ssize_t consumed = nghttp2_session_mem_recv(
      session_, read_buf_.data(), static_cast<size_t>(nread));
  if (consumed < 0) {
    if (consumed == NGHTTP2_ERR_FLOODED) {
      // nghttp2 detected a protocol-level flood — hard close.
    }
    close();
    return;
  }

  reset_idle_timer();
  do_write();
}

// do_write() — drain nghttp2 outbound queue to socket.
void Connection::do_write() {
  if (closed_) return;
  write_signaled_ = false;

  // ---- Phase 1: drain any residual from a previous partial write ----
  while (write_buf_offset_ < write_buf_len_) {
    ssize_t n = ::write(fd_, write_buf_.data() + write_buf_offset_,
                        write_buf_len_ - write_buf_offset_);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (!write_pending_) {
          loop_.modify_fd(fd_, EPOLLIN | EPOLLOUT | EPOLLRDHUP);
          write_pending_ = true;
        }
        return;
      }
      close();
      return;
    }
    write_buf_offset_ += static_cast<size_t>(n);
  }

  // ---- Phase 2: fetch new frames and write them ----
  while (nghttp2_session_want_write(session_) || overflow_buf_) {
    if (fill_write_buf() != 0) {
      close();
      return;
    }
    if (write_buf_len_ == 0) break;

    while (write_buf_offset_ < write_buf_len_) {
      ssize_t n = ::write(fd_, write_buf_.data() + write_buf_offset_,
                          write_buf_len_ - write_buf_offset_);
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          if (!write_pending_) {
            loop_.modify_fd(fd_, EPOLLIN | EPOLLOUT | EPOLLRDHUP);
            write_pending_ = true;
          }
          return;
        }
        close();
        return;
      }
      write_buf_offset_ += static_cast<size_t>(n);
    }
  }

  // ---- All pending data written ----
  if (write_pending_) {
    loop_.modify_fd(fd_, EPOLLIN | EPOLLRDHUP);
    write_pending_ = false;
  }

  // If nghttp2 no longer needs to read or write, the session is done.
  if (!nghttp2_session_want_read(session_) &&
      !nghttp2_session_want_write(session_)) {
    close();
  }
}

// signal_write() — post a deferred do_write() to the owning EventLoop.
// Using shared_from_this() ensures the Connection stays alive until do_write()
// executes. The write_signaled_ flag prevents duplicate posts.
void Connection::signal_write() {
  if (closed_) return;
  if (!write_signaled_) {
    write_signaled_ = true;
    loop_.post([self = shared_from_this()]() { self->do_write(); });
  }
}

// reset_idle_timer() — re-arm the idle timerfd on each received frame.
void Connection::reset_idle_timer() {
  if (idle_timer_fd_ >= 0) {
    loop_.reset_timer(idle_timer_fd_, kIdleTimeoutMs);
  }
}

// ---------------------------------------------------------------------------
// on_epoll_event()
// ---------------------------------------------------------------------------

void Connection::on_epoll_event(uint32_t events) {
  if (closed_) return;
  if (events & (EPOLLERR | EPOLLHUP)) {
    close();
    return;
  }
  // EPOLLRDHUP (peer half-close) is handled by do_read() returning nread==0.
  if (events & (EPOLLIN | EPOLLRDHUP)) do_read();
  if (!closed_ && (events & EPOLLOUT))  do_write();
}

// ---------------------------------------------------------------------------
// submit_response() — v1 API
// ---------------------------------------------------------------------------

void Connection::submit_response(int32_t stream_id, uint16_t status_code,
                                  const std::multimap<std::string, std::string>& headers,
                                  std::string body) {
  auto status_str = std::to_string(status_code);

  std::vector<nghttp2_nv> nva;
  nva.reserve(1 + headers.size());
  // ":status" pseudo-header must come first.
  nva.push_back({(uint8_t*)":status", (uint8_t*)status_str.c_str(),
                 7, status_str.size(), NGHTTP2_NV_FLAG_NONE});

  for (auto& [k, v] : headers) {
    nva.push_back({(uint8_t*)k.data(), (uint8_t*)v.data(),
                   k.size(), v.size(), NGHTTP2_NV_FLAG_NONE});
  }

  nghttp2_data_provider* prd_ptr = nullptr;
  nghttp2_data_provider  prd{};
  if (!body.empty()) {
    response_bodies_[stream_id]        = std::move(body);
    response_body_offsets_[stream_id]  = 0;
    prd.source.ptr   = this;
    prd.read_callback = data_provider_read_cb;
    prd_ptr = &prd;
  }

  nghttp2_submit_response(session_, stream_id, nva.data(), nva.size(), prd_ptr);
  signal_write();
}

// ---------------------------------------------------------------------------
// read_response_data() — delivers response body bytes to nghttp2
// ---------------------------------------------------------------------------

ssize_t Connection::read_response_data(int32_t stream_id, uint8_t* buf,
                                       size_t length, uint32_t* data_flags) {
  auto body_it = response_bodies_.find(stream_id);
  if (body_it == response_bodies_.end()) {
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return 0;
  }

  auto offset_it = response_body_offsets_.find(stream_id);
  size_t offset  = (offset_it != response_body_offsets_.end()) ?
                    offset_it->second : 0;
  const std::string& body_ref = body_it->second;
  size_t remaining = body_ref.size() - offset;

  if (remaining == 0) {
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    response_bodies_.erase(body_it);
    response_body_offsets_.erase(stream_id);
    return 0;
  }

  size_t to_copy = std::min(remaining, length);
  std::memcpy(buf, body_ref.data() + offset, to_copy);
  response_body_offsets_[stream_id] = offset + to_copy;

  if (offset + to_copy >= body_ref.size()) {
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    response_bodies_.erase(body_it);
    response_body_offsets_.erase(stream_id);
  }

  return static_cast<ssize_t>(to_copy);
}

// ---------------------------------------------------------------------------
// nghttp2 callbacks
// ---------------------------------------------------------------------------

// Callback 1: HEADERS frame begins — create StreamData for request streams.
int Connection::on_begin_headers_cb(nghttp2_session* /*session*/,
                                    const nghttp2_frame* frame,
                                    void* user_data) {
  auto* conn = static_cast<Connection*>(user_data);
  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return 0;
  }
  conn->create_stream(frame->hd.stream_id);
  return 0;
}

// Callback 2: Individual header field — populate StreamData.
int Connection::on_header_cb(nghttp2_session* session,
                             const nghttp2_frame* frame,
                             const uint8_t* name,  size_t namelen,
                             const uint8_t* value, size_t valuelen,
                             uint8_t /*flags*/, void* user_data) {
  auto* conn = static_cast<Connection*>(user_data);
  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return 0;
  }
  auto* strm = conn->find_stream(frame->hd.stream_id);
  if (!strm) return 0;

  std::string_view n(reinterpret_cast<const char*>(name), namelen);
  std::string      v_str(reinterpret_cast<const char*>(value), valuelen);

  if (n == ":method") {
    strm->method = std::move(v_str);
  } else if (n == ":path") {
    auto qpos = v_str.find('?');
    if (qpos != std::string::npos) {
      strm->raw_query = v_str.substr(qpos + 1);
      strm->path      = v_str.substr(0, qpos);
    } else {
      strm->path = std::move(v_str);
    }
  } else if (n == ":scheme") {
    strm->scheme = std::move(v_str);
  } else if (n == ":authority") {
    strm->authority = std::move(v_str);
  } else {
    // HPACK bomb protection: reject requests whose decompressed header set
    // exceeds kMaxHeaderListSize.
    if (strm->header_buffer_size + namelen + valuelen > kMaxHeaderListSize) {
      nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE,
                                frame->hd.stream_id, NGHTTP2_INTERNAL_ERROR);
      return 0;
    }
    strm->header_buffer_size += namelen + valuelen;
    strm->headers.emplace(std::string(n), std::move(v_str));
  }
  return 0;
}

// Callback 3: Complete frame received — dispatch request on END_STREAM.
int Connection::on_frame_recv_cb(nghttp2_session* /*session*/,
                                 const nghttp2_frame* frame,
                                 void* user_data) {
  auto* conn = static_cast<Connection*>(user_data);
  auto* strm = conn->find_stream(frame->hd.stream_id);

  switch (frame->hd.type) {
    case NGHTTP2_DATA:
      if (strm && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        if (strm->body_limit_exceeded) {
          // Body exceeded kMaxRequestBodySize.  Send RST_STREAM to the client
          // and clean up without dispatching the request.
          nghttp2_submit_rst_stream(conn->session_, NGHTTP2_FLAG_NONE,
                                    frame->hd.stream_id,
                                    NGHTTP2_INTERNAL_ERROR);
          conn->close_stream(frame->hd.stream_id);
        } else {
          conn->dispatch_request(*strm);
        }
      }
      break;
    case NGHTTP2_HEADERS:
      if (!strm || frame->headers.cat != NGHTTP2_HCAT_REQUEST) break;
      strm->headers_complete = true;
      if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
        conn->dispatch_request(*strm);
      }
      break;
    default:
      break;
  }
  return 0;
}

// Callback 4: DATA chunk — accumulate request body with size guard.
int Connection::on_data_chunk_recv_cb(nghttp2_session* session,
                                      uint8_t /*flags*/,
                                      int32_t stream_id,
                                      const uint8_t* data, size_t len,
                                      void* user_data) {
  auto* conn = static_cast<Connection*>(user_data);
  auto* strm = conn->find_stream(stream_id);
  if (!strm) return 0;

  if (strm->body_limit_exceeded ||
      strm->body.size() + len > kMaxRequestBodySize) {
    // Body limit exceeded.  Mark the stream so on_frame_recv_cb can send an
    // explicit RST_STREAM instead of dispatching the request.  The window is
    // still consumed to avoid stalling the peer before END_STREAM arrives.
    strm->body_limit_exceeded = true;
    nghttp2_session_consume(session, stream_id, len);
    return 0;
  }
  strm->body.append(reinterpret_cast<const char*>(data), len);
  // Restore the flow-control window so the peer can continue sending request
  // body up to kMaxRequestBodySize.  With no_auto_window_update=1 this call
  // is mandatory; without it the stream window is never restored and clients
  // cannot send more than kInitialWindowSize (65535) bytes of body.
  nghttp2_session_consume(session, stream_id, len);
  return 0;
}

// Callback 5: Stream closed — clean up state and check drain completion.
int Connection::on_stream_close_cb(nghttp2_session* /*session*/,
                                   int32_t stream_id,
                                   uint32_t /*error_code*/,
                                   void* user_data) {
  auto* conn = static_cast<Connection*>(user_data);
  conn->close_stream(stream_id);
  --conn->active_streams_;
  if (conn->shutdown_state_ == ShutdownState::DRAINING &&
      conn->active_streams_ == 0) {
    conn->close();
  }
  return 0;
}

// Callback 6: Frame sent — no-op for NRF (no push promise).
int Connection::on_frame_send_cb(nghttp2_session* /*session*/,
                                 const nghttp2_frame* /*frame*/,
                                 void* /*user_data*/) {
  return 0;
}

// Callback 7: Frame not sent — RST_STREAM unresponsive HEADERS.
int Connection::on_frame_not_send_cb(nghttp2_session* session,
                                     const nghttp2_frame* frame,
                                     int /*lib_error_code*/,
                                     void* /*user_data*/) {
  if (frame->hd.type != NGHTTP2_HEADERS) return 0;
  nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE,
                            frame->hd.stream_id, NGHTTP2_INTERNAL_ERROR);
  return 0;
}

// Static trampoline: delegates to Connection::read_response_data().
ssize_t Connection::data_provider_read_cb(nghttp2_session* /*session*/,
                                          int32_t stream_id,
                                          uint8_t* buf, size_t length,
                                          uint32_t* data_flags,
                                          nghttp2_data_source* /*source*/,
                                          void* user_data) {
  auto* conn = static_cast<Connection*>(user_data);
  return conn->read_response_data(stream_id, buf, length, data_flags);
}

// ---------------------------------------------------------------------------
// Stream management
// ---------------------------------------------------------------------------

StreamData* Connection::create_stream(int32_t stream_id) {
  auto [it, ok] = streams_.emplace(stream_id, StreamData{});
  if (!ok) return nullptr;
  it->second.stream_id = stream_id;
  // Increment here so active_streams_ always matches the number of open
  // streams.  Decrementing on every stream-close (on_stream_close_cb) is now
  // safe because the counts are symmetric: one increment per create, one
  // decrement per close, even if END_STREAM never arrives (reset/error).
  ++active_streams_;
  return &it->second;
}

StreamData* Connection::find_stream(int32_t stream_id) {
  auto it = streams_.find(stream_id);
  return it != streams_.end() ? &it->second : nullptr;
}

void Connection::close_stream(int32_t stream_id) {
  streams_.erase(stream_id);
  response_bodies_.erase(stream_id);
  response_body_offsets_.erase(stream_id);
}

void Connection::dispatch_request(StreamData& strm) {
  // active_streams_ was already incremented in create_stream(); no increment
  // here avoids double-counting if close fires before dispatch completes.
  RequestHandler handler = router_.match(strm.path);
  if (!handler) {
    submit_response(strm.stream_id, 404,
                    {{"content-type", "text/plain"}}, "Not Found");
    return;
  }
  // Request/Response constructors are defined in h2_request.cpp / h2_response.cpp
  // which include h2_connection.hpp for complete StreamData and Connection types.
  Request  req(strm);
  Response res(shared_from_this(), strm.stream_id);
  handler(req, res);
}

}  // namespace h2
