// test_h2_thread_pool.cpp
// Unit tests for h2::ThreadPool (Task 1.2).
// Validates acceptance criteria from the review feedback:
//   1. ThreadPool(4) creates exactly 4 workers (size() == 4).
//   2. start() brings all EventLoops to running state.
//   3. get_next_loop() distributes work across 4 distinct worker thread IDs.
//   4. stop() joins all threads cleanly within a 2-second deadline.
// Bonus: double-start / double-stop idempotency.

#include <gtest/gtest.h>
#include "h2_thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>

// ---------------------------------------------------------------------------
// Helper: spin-wait until `cond()` returns true or `timeout` elapses.
// Returns true when the condition was satisfied.
// ---------------------------------------------------------------------------
template<typename Pred>
static bool spin_wait(
    Pred cond,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{2000})
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!cond()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return true;
}

// ---------------------------------------------------------------------------
// Helper: wait until all loops in the pool report is_running().
// ---------------------------------------------------------------------------
static bool wait_all_running(h2::ThreadPool& pool,
                             std::chrono::milliseconds timeout =
                                 std::chrono::milliseconds{2000})
{
    return spin_wait([&] {
        for (auto& loop : pool.loops()) {
            if (!loop->is_running()) return false;
        }
        return true;
    }, timeout);
}

// ===========================================================================
// Test 1: size() == 4 after construction
// ===========================================================================
TEST(ThreadPool, SizeMatchesRequestedWorkers) {
    h2::ThreadPool pool(4);
    EXPECT_EQ(pool.size(), 4u);
}

// ===========================================================================
// Test 2: start() — all EventLoops reach running state
// ===========================================================================
TEST(ThreadPool, StartSetsAllLoopsRunning) {
    h2::ThreadPool pool(4);
    pool.start();

    EXPECT_TRUE(wait_all_running(pool))
        << "Not all EventLoops reached is_running()==true within 2 s";

    pool.stop();

    // After stop, no loop should report running.
    for (auto& loop : pool.loops()) {
        EXPECT_FALSE(loop->is_running())
            << "EventLoop still reports running after stop()";
    }
}

// ===========================================================================
// Test 3: get_next_loop() round-robin — 4 tasks execute on 4 distinct thread IDs
// ===========================================================================
TEST(ThreadPool, GetNextLoopYieldsDistinctWorkerThreadIds) {
    h2::ThreadPool pool(4);
    pool.start();
    ASSERT_TRUE(wait_all_running(pool)) << "Pool did not start within 2 s";

    std::mutex           mu;
    std::set<std::thread::id> worker_ids;
    std::atomic<int>     done{0};

    // Post one task to each worker via consecutive get_next_loop() calls.
    // Round-robin index starts at 0, so 4 calls cover loops 0..3 exactly once.
    for (size_t i = 0; i < pool.size(); ++i) {
        pool.get_next_loop().post([&mu, &worker_ids, &done] {
            std::thread::id tid = std::this_thread::get_id();
            {
                std::lock_guard<std::mutex> lk(mu);
                worker_ids.insert(tid);
            }
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Wait for all 4 tasks to finish.
    bool all_done = spin_wait([&] {
        return done.load(std::memory_order_relaxed) == static_cast<int>(pool.size());
    });
    EXPECT_TRUE(all_done) << "Posted tasks did not complete within 2 s";

    // Each loop runs in its own thread → 4 distinct IDs.
    EXPECT_EQ(worker_ids.size(), pool.size())
        << "Expected " << pool.size() << " distinct worker thread IDs, "
        << "got " << worker_ids.size();

    // None of the worker IDs should be the main thread.
    EXPECT_EQ(worker_ids.count(std::this_thread::get_id()), 0u)
        << "Main thread executed a worker task — loop was not running";

    pool.stop();
}

// ===========================================================================
// Test 4: stop() joins all threads within 2 seconds
// ===========================================================================
TEST(ThreadPool, StopJoinsAllThreadsWithinDeadline) {
    h2::ThreadPool pool(4);
    pool.start();
    ASSERT_TRUE(wait_all_running(pool)) << "Pool did not start within 2 s";

    auto t0 = std::chrono::steady_clock::now();
    pool.stop();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0);

    EXPECT_LT(elapsed.count(), 2000)
        << "stop() took " << elapsed.count() << " ms (limit 2000 ms)";

    // All loops must be idle after a clean stop.
    for (auto& loop : pool.loops()) {
        EXPECT_FALSE(loop->is_running())
            << "EventLoop still running after stop()";
    }
}

// ===========================================================================
// Bonus: double-start is idempotent (shouldn't spawn extra threads)
// ===========================================================================
TEST(ThreadPool, DoubleStartIsIdempotent) {
    h2::ThreadPool pool(2);
    pool.start();
    pool.start();  // second call must be a no-op

    EXPECT_TRUE(wait_all_running(pool));
    EXPECT_EQ(pool.size(), 2u);  // still 2 loops, not 4
    pool.stop();
}

// ===========================================================================
// Bonus: double-stop is idempotent (shouldn't crash/hang)
// ===========================================================================
TEST(ThreadPool, DoubleStopIsIdempotent) {
    h2::ThreadPool pool(2);
    pool.start();
    // Wait for threads to reach run() before calling stop(); otherwise the
    // thread could overwrite running_=false with running_=true and hang.
    ASSERT_TRUE(wait_all_running(pool));
    pool.stop();
    pool.stop();   // second call: started_==false, must be a no-op
    SUCCEED();
}

// ===========================================================================
// Bonus: destructor calls stop() automatically (no leak / no hang)
// ===========================================================================
TEST(ThreadPool, DestructorStopsRunningPool) {
    // If destructor doesn't stop cleanly this test would hang/crash.
    {
        h2::ThreadPool pool(2);
        pool.start();
        ASSERT_TRUE(wait_all_running(pool));
        // pool destroyed here — destructor must call stop()
    }
    SUCCEED();
}

// ===========================================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
