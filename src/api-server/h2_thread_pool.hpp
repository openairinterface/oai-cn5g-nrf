// h2_thread_pool.hpp
#pragma once
#include "h2_event_loop.hpp"
#include <vector>
#include <thread>
#include <memory>
#include <atomic>
#include <cstddef>

namespace h2 {

// Manages N worker threads, each running its own EventLoop.
// New connections are distributed round-robin across workers via get_next_loop().
//
// Thread-safety:
//   - start() / stop() must be called from the main (coordinator) thread.
//   - get_next_loop() is thread-safe (atomic round-robin counter).
//   - loops() is not thread-safe; use only before start() or after stop().
class ThreadPool {
 public:
  // Construct with num_threads workers. If num_threads == 0, defaults to
  // std::thread::hardware_concurrency() (minimum 1).
  explicit ThreadPool(size_t num_threads = 0);
  ~ThreadPool();

  ThreadPool(const ThreadPool&)            = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&)                 = delete;
  ThreadPool& operator=(ThreadPool&&)      = delete;

  // Launch one thread per EventLoop. Each thread runs EventLoop::run()
  // and blocks until stop() is called.
  void start();

  // Signal all EventLoops to stop (via EventLoop::stop()), then join all
  // worker threads. Safe to call multiple times; no-op if already stopped.
  void stop();

  // Return a reference to the next EventLoop in round-robin order.
  // Thread-safe. Loops are stable (never reallocated after construction).
  EventLoop& get_next_loop();

  // Number of worker EventLoops.
  size_t size() const { return loops_.size(); }

  // Direct access to all loops (e.g. for shutdown coordination).
  // Must not be called concurrently with start() or stop().
  std::vector<std::unique_ptr<EventLoop>>& loops() { return loops_; }

 private:
  std::vector<std::unique_ptr<EventLoop>> loops_;
  std::vector<std::thread>                threads_;
  std::atomic<size_t>                     next_loop_{0};
  bool                                    started_{false};
};

}  // namespace h2
