#pragma once
#include <atomic>
#include <functional>
#include <cstdint>
#include <memory>
#include <set>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace h2 {

class Connection;  // forward declaration for per-worker ownership

using IoCallback = std::function<void(uint32_t events)>;

// Per-thread event loop wrapping Linux epoll (level-triggered only).
// Each EventLoop owns one epoll_fd, one event_fd (eventfd for cross-thread
// wakeup), and zero or more timer_fds.
//
// Thread-safety:
//   - run()          must be called from exactly one thread.
//   - post(), stop() are safe to call from any thread.
//   - add_fd(), modify_fd(), remove_fd(), timer ops must be called from the
//     owning thread (or scheduled via post()).
//
// NOTE: Level-triggered epoll only.
class EventLoop {
 public:
  EventLoop();
  ~EventLoop();

  // Non-copyable, non-movable (owns file descriptors).
  EventLoop(const EventLoop&)            = delete;
  EventLoop& operator=(const EventLoop&) = delete;
  EventLoop(EventLoop&&)                 = delete;
  EventLoop& operator=(EventLoop&&)      = delete;

  // Register fd for monitoring.
  // events: EPOLLIN | EPOLLOUT | EPOLLRDHUP etc. (level-triggered).
  bool add_fd(int fd, uint32_t events, IoCallback cb);

  // Change the watched events for an already-registered fd.
  bool modify_fd(int fd, uint32_t events);

  // Deregister fd and remove its callback.
  bool remove_fd(int fd);

  // Create a timerfd that fires after timeout_ms milliseconds.
  // If oneshot is true (default), the timer fires once; otherwise it repeats.
  // Returns the timer_fd on success, -1 on error.
  // The fd is automatically registered with epoll (EPOLLIN, level-triggered).
  int add_timer(uint64_t timeout_ms, IoCallback cb, bool oneshot = true);

  // Remove timerfd from epoll and close it.
  void cancel_timer(int timer_fd);

  // Rearm an existing timerfd to fire after timeout_ms milliseconds.
  // Returns true on success, false on failure (logs warning; does not throw).
  bool reset_timer(int timer_fd, uint64_t timeout_ms);

  // Post a callable to this event loop from any thread (thread-safe).
  // The callable is executed in the event loop's owning thread on the next
  // epoll_wait wakeup.
  void post(std::function<void()> fn);

  // Run the event loop (blocking). Returns when stop() is called.
  void run();

  // Signal the loop to exit (thread-safe). Wakes epoll_wait via eventfd.
  void stop();

  // Set running_=true before the thread that will call run() is spawned.
  // Must be called by the owner (e.g. ThreadPool::start()) prior to thread
  // creation so that stop()-before-run() is safe: if stop() fires between
  // arm() and run(), running_ is false and run() exits immediately.
  void arm();

  bool is_running() const {
    return running_.load(std::memory_order_relaxed);
  }

  // ── Per-worker connection ownership (C3 fix: no global connection set) ──
  // All mutation occurs on the owning thread — no mutex needed.
  void add_connection(int fd, std::shared_ptr<Connection> conn);
  void remove_connection(int fd);
  std::unordered_map<int, std::shared_ptr<Connection>>& connections();
  const std::unordered_map<int, std::shared_ptr<Connection>>& connections() const;
  bool shutting_down() const;
  void set_shutting_down(bool v);

 private:
  // Drain the eventfd counter and execute all pending posted functions.
  void handle_event_fd();

  int epoll_fd_;  // epoll_create1(EPOLL_CLOEXEC)
  int event_fd_;  // eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)

  std::atomic<bool> running_{false};

  // fd → callback map (only accessed from owning thread during run()).
  std::unordered_map<int, IoCallback> fd_callbacks_;

  // Timer fds created internally by add_timer() — owned and closed by this
  // EventLoop. Fds registered via add_fd() are caller-owned and never closed
  // here.
  std::set<int> timer_fds_;

  // Cross-thread posted function queue.
  std::mutex                          post_mutex_;
  std::vector<std::function<void()>>  posted_fns_;
  std::vector<std::function<void()>>  processing_fns_;  // swap buffer

  // Per-worker connection map — keyed by socket fd for O(1) removal.
  std::unordered_map<int, std::shared_ptr<Connection>> connections_;
  bool shutting_down_ = false;

  static constexpr int kMaxEvents = 64;
};

}  // namespace h2
