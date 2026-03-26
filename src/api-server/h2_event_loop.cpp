#include "h2_event_loop.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace h2 {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

EventLoop::EventLoop()
    : epoll_fd_(-1), event_fd_(-1) {
  // Create epoll instance with CLOEXEC so child processes don't inherit it.
  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "epoll_create1 failed");
  }

  // Create eventfd for cross-thread wakeup (non-blocking + close-on-exec).
  event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (event_fd_ < 0) {
    ::close(epoll_fd_);
    throw std::system_error(errno, std::generic_category(),
                            "eventfd failed");
  }

  // Register event_fd_ with epoll — level-triggered EPOLLIN.
  // epoll keeps reporting EPOLLIN until the eventfd counter is drained.
  struct epoll_event ev{};
  ev.events  = EPOLLIN;  // level-triggered
  ev.data.fd = event_fd_;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &ev) < 0) {
    ::close(event_fd_);
    ::close(epoll_fd_);
    throw std::system_error(errno, std::generic_category(),
                            "epoll_ctl(ADD, event_fd_) failed");
  }

  fd_callbacks_[event_fd_] = [this](uint32_t /*events*/) {
    handle_event_fd();
  };
}

EventLoop::~EventLoop() {
  // Close only timer fds — those are created internally by add_timer() and
  // are owned by this EventLoop. Fds registered via add_fd() are owned by the
  // caller (e.g. Connection objects) and must not be closed here.
  for (int tfd : timer_fds_) {
    ::close(tfd);
  }
  fd_callbacks_.clear();

  if (event_fd_ >= 0) ::close(event_fd_);
  if (epoll_fd_ >= 0) ::close(epoll_fd_);
}

// ---------------------------------------------------------------------------
// fd registration
// ---------------------------------------------------------------------------

bool EventLoop::add_fd(int fd, uint32_t events, IoCallback cb) {
  struct epoll_event ev{};
  ev.events  = events;
  ev.data.fd = fd;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
    return false;
  }
  fd_callbacks_[fd] = std::move(cb);
  return true;
}

bool EventLoop::modify_fd(int fd, uint32_t events) {
  struct epoll_event ev{};
  ev.events  = events;
  ev.data.fd = fd;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
    return false;
  }
  return true;
}

bool EventLoop::remove_fd(int fd) {
  auto it = fd_callbacks_.find(fd);
  if (it == fd_callbacks_.end()) {
    // Unknown fd — nothing to do.
    return false;
  }
  // Remove from epoll first; only then clean up the callback map.
  // If we erased the callback first and EPOLL_CTL_DEL failed, epoll would
  // still watch the fd with no handler — a dangling watch.
  // epoll_ctl with EPOLL_CTL_DEL ignores the event argument (Linux ≥ 2.6.9).
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
    // EBADF/ENOENT: fd is already invalid or not registered — epoll state
    // already reflects removal.  Erase callback to keep our state consistent.
    // Other errors are unexpected; log them but still erase for consistency.
    if (errno != EBADF && errno != ENOENT) {
      std::fprintf(stderr, "[h2::EventLoop] EPOLL_CTL_DEL fd=%d failed: %s\n",
                   fd, std::strerror(errno));
    }
    fd_callbacks_.erase(it);
    return false;
  }
  fd_callbacks_.erase(it);
  return true;
}

// ---------------------------------------------------------------------------
// Timer management
// ---------------------------------------------------------------------------

static bool set_timerfd(int tfd, uint64_t timeout_ms, bool oneshot) {
  struct itimerspec spec{};
  spec.it_value.tv_sec  = static_cast<time_t>(timeout_ms / 1000u);
  spec.it_value.tv_nsec = static_cast<long>((timeout_ms % 1000u) * 1'000'000L);
  if (!oneshot) {
    // Repeating: set interval equal to initial value.
    spec.it_interval = spec.it_value;
  }
  // If timeout_ms == 0 a zero it_value would disarm the timer; treat as 1 ms.
  if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0) {
    spec.it_value.tv_nsec = 1'000'000L;  // 1 ms minimum
    if (!oneshot) spec.it_interval = spec.it_value;
  }
  return ::timerfd_settime(tfd, 0, &spec, nullptr) == 0;
}

int EventLoop::add_timer(uint64_t timeout_ms, IoCallback cb, bool oneshot) {
  int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (tfd < 0) {
    return -1;
  }

  if (!set_timerfd(tfd, timeout_ms, oneshot)) {
    ::close(tfd);
    return -1;
  }

  // Register with epoll — level-triggered EPOLLIN.
  struct epoll_event ev{};
  ev.events  = EPOLLIN;  // level-triggered
  ev.data.fd = tfd;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, tfd, &ev) < 0) {
    ::close(tfd);
    return -1;
  }
  timer_fds_.insert(tfd);
  fd_callbacks_[tfd] = std::move(cb);
  return tfd;
}

void EventLoop::cancel_timer(int timer_fd) {
  remove_fd(timer_fd);
  timer_fds_.erase(timer_fd);
  ::close(timer_fd);
}

bool EventLoop::reset_timer(int timer_fd, uint64_t timeout_ms) {
  // Determine oneshot vs repeating: check existing itimerspec.
  struct itimerspec cur{};
  if (::timerfd_gettime(timer_fd, &cur) < 0) {
    std::fprintf(stderr, "[h2::EventLoop] timerfd_gettime fd=%d failed: %s\n",
                 timer_fd, std::strerror(errno));
    return false;
  }
  bool repeating =
      (cur.it_interval.tv_sec != 0 || cur.it_interval.tv_nsec != 0);
  if (!set_timerfd(timer_fd, timeout_ms, !repeating)) {
    std::fprintf(stderr, "[h2::EventLoop] timerfd_settime fd=%d failed: %s\n",
                 timer_fd, std::strerror(errno));
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Cross-thread post
// ---------------------------------------------------------------------------

void EventLoop::post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lk(post_mutex_);
    posted_fns_.push_back(std::move(fn));
  }
  // Wake epoll_wait by incrementing the eventfd counter.
  const uint64_t one = 1;
  // Ignore return value; EFD_NONBLOCK means it won't block and EAGAIN (counter
  // at max UINT64_MAX-1) is extremely unlikely.
  (void) ::write(event_fd_, &one, sizeof(one));
}

// ---------------------------------------------------------------------------
// handle_event_fd — called from owning thread inside epoll_wait loop
// ---------------------------------------------------------------------------

void EventLoop::handle_event_fd() {
  // Drain the eventfd counter (a single read returns the current value and
  // resets it to zero; level-triggered epoll stops firing after this).
  uint64_t counter = 0;
  // Loop to handle the race where another write arrives between read and drain.
  // In practice a single read suffices, but we loop for correctness.
  while (::read(event_fd_, &counter, sizeof(counter)) > 0) {
    // drained
  }
  // errno == EAGAIN when the counter is 0 — that's normal, stop reading.

  // Swap under lock so post() callers get an empty vector immediately.
  {
    std::lock_guard<std::mutex> lk(post_mutex_);
    processing_fns_.swap(posted_fns_);
  }

  // Execute all pending callables outside the lock.
  for (auto& fn : processing_fns_) {
    fn();
  }
  processing_fns_.clear();
}

// ---------------------------------------------------------------------------
// run / stop
// ---------------------------------------------------------------------------

void EventLoop::arm() {
  // Called by the owner thread before spawning the worker thread.
  // Use release ordering to synchronize with the acquire load in run(),
  // ensuring all prior writes are visible to the worker thread.
  running_.store(true, std::memory_order_release);
}

void EventLoop::run() {
  // Do NOT set running_=true here. arm() must be called before this thread
  // is spawned so that a stop()-before-run() race is handled correctly:
  // if stop() fires between arm() and run(), running_ is already false and
  // the while loop below never executes, letting join() proceed normally.

  struct epoll_event events[kMaxEvents];

  while (running_.load(std::memory_order_acquire)) {
    int n = ::epoll_wait(epoll_fd_, events, kMaxEvents, -1 /*block forever*/);

    if (n < 0) {
      if (errno == EINTR) {
        // Signal interrupted epoll_wait — not an error, just retry.
        continue;
      }
      // Other error: log and exit the loop.
      break;
    }

    for (int i = 0; i < n; ++i) {
      int fd = events[i].data.fd;
      auto it = fd_callbacks_.find(fd);
      if (it != fd_callbacks_.end()) {
        it->second(events[i].events);
      }
      // If fd was removed inside the callback fd_callbacks_ no longer has it,
      // which is fine — we already captured 'it' above.
    }
  }

  running_.store(false, std::memory_order_relaxed);
}

void EventLoop::stop() {
  running_.store(false, std::memory_order_release);
  // Wake the blocking epoll_wait so run() can check running_ and exit.
  const uint64_t one = 1;
  (void) ::write(event_fd_, &one, sizeof(one));
}

// ---------------------------------------------------------------------------
// Per-worker connection ownership
// ---------------------------------------------------------------------------

void EventLoop::add_connection(int fd, std::shared_ptr<Connection> conn) {
  connections_.emplace(fd, std::move(conn));
}

void EventLoop::remove_connection(int fd) {
  connections_.erase(fd);
}

std::unordered_map<int, std::shared_ptr<Connection>>& EventLoop::connections() {
  return connections_;
}

const std::unordered_map<int, std::shared_ptr<Connection>>& EventLoop::connections() const {
  return connections_;
}

bool EventLoop::shutting_down() const { return shutting_down_; }
void EventLoop::set_shutting_down(bool v) { shutting_down_ = v; }

}  // namespace h2
