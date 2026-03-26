// h2_thread_pool.cpp
#include "h2_thread_pool.hpp"
#include <algorithm>
#include <stdexcept>
#include <thread>

namespace h2 {

ThreadPool::ThreadPool(size_t num_threads) {
  if (num_threads == 0) {
    num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 1;
  }
  loops_.reserve(num_threads);
  for (size_t i = 0; i < num_threads; ++i) {
    loops_.emplace_back(std::make_unique<EventLoop>());
  }
  threads_.reserve(num_threads);
}

ThreadPool::~ThreadPool() {
  stop();
}

void ThreadPool::start() {
  if (started_) return;
  started_ = true;

  // Capture a stable raw pointer per loop.
  // SAFETY: loops_ is fully populated and never reallocated after construction,
  // so raw pointers remain valid for the lifetime of this ThreadPool.
  // Capturing [&loop] in a range-for would bind all threads to the last
  // iteration's reference — a data race. We capture by value instead.
  for (auto& loop : loops_) {
    EventLoop* lp = loop.get();
    // arm() sets running_=true BEFORE the thread is spawned.
    // If stop() fires before the worker enters run(), running_ will be false
    // and run() returns immediately, making join() safe.
    lp->arm();
    threads_.emplace_back([lp] { lp->run(); });
  }
}

void ThreadPool::stop() {
  if (!started_) return;
  started_ = false;

  // Signal all loops first so they can unblock concurrently.
  for (auto& loop : loops_) {
    loop->stop();
  }

  // Join threads in order.
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
  threads_.clear();
}

EventLoop& ThreadPool::get_next_loop() {
  // fetch_add is relaxed — we only need atomicity, not ordering.
  size_t idx = next_loop_.fetch_add(1, std::memory_order_relaxed) % loops_.size();
  return *loops_[idx];
}

}  // namespace h2
