/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_THREAD_POOL_SEEN
#define FILE_THREAD_POOL_SEEN

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// Standalone Thread Pool
// A simple fixed-size thread pool with a bounded task queue.
// No HTTP/2 or libevent dependencies — reusable across OAI NFs.
//
// Usage:
//   thread_pool pool(4, 1000);            // 4 workers, max 1000 queued tasks
//   bool ok = pool.enqueue([](){ ... });  // submit work; false if
//   full/shutdown pool.shutdown();                      // drain + join
//   (destructor also calls)

class thread_pool {
 public:
  // Create a pool with num_threads workers and an optional max queue depth.
  // Workers start immediately.
  explicit thread_pool(size_t num_threads, size_t max_queue_size = 100000)
      : shutdown_(false), max_queue_size_(max_queue_size) {
    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  }

  ~thread_pool() { shutdown(); }

  // Non-copyable, non-movable
  thread_pool(const thread_pool&) = delete;
  thread_pool& operator=(const thread_pool&) = delete;
  thread_pool(thread_pool&&)                 = delete;
  thread_pool& operator=(thread_pool&&) = delete;

  // Enqueue a task for execution on a worker thread.
  // Returns false if the pool is shut down or the queue is full.
  // Thread-safe — can be called from any thread.
  bool enqueue(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (shutdown_) {
        enqueue_rejections_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      if (max_queue_size_ > 0 && tasks_.size() >= max_queue_size_) {
        enqueue_rejections_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      tasks_.push(std::move(task));
      queued_tasks_.fetch_add(1, std::memory_order_relaxed);
    }
    cv_.notify_one();
    return true;
  }

  // Signal workers to stop accepting new tasks and drain already queued work.
  // Does not join worker threads; use shutdown() for the blocking join phase.
  // Safe to call multiple times and from the event-loop thread.
  void stop_accepting() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (shutdown_) return;
      shutdown_ = true;
    }
    cv_.notify_all();
  }

  // Signal all workers to stop, drain remaining tasks, and join threads.
  // Blocks until all workers have exited.  Safe to call multiple times.
  void shutdown() {
    stop_accepting();
    for (auto& w : workers_) {
      if (w.joinable()) w.join();
    }
  }

  // Returns true after shutdown() has been called.
  // Lock-free read — use as a hint; enqueue() is authoritative under mutex.
  bool is_shutdown() const { return shutdown_; }

  size_t queued_tasks() const {
    return queued_tasks_.load(std::memory_order_relaxed);
  }

  size_t active_tasks() const {
    return active_tasks_.load(std::memory_order_relaxed);
  }

  size_t max_queue_size() const { return max_queue_size_; }

  uint64_t enqueue_rejections() const {
    return enqueue_rejections_.load(std::memory_order_relaxed);
  }

 private:
  void worker_loop() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return shutdown_ || !tasks_.empty(); });
        if (shutdown_ && tasks_.empty()) return;
        task = std::move(tasks_.front());
        tasks_.pop();
        queued_tasks_.fetch_sub(1, std::memory_order_relaxed);
      }
      active_tasks_.fetch_add(1, std::memory_order_relaxed);
      task();
      active_tasks_.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> shutdown_;
  size_t max_queue_size_;
  std::atomic<size_t> queued_tasks_{0};
  std::atomic<size_t> active_tasks_{0};
  std::atomic<uint64_t> enqueue_rejections_{0};
};

#endif  // FILE_THREAD_POOL_SEEN
