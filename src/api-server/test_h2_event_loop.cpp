// test_h2_event_loop.cpp
// Unit tests for h2::EventLoop (Task 4.1).
//
// Tests: construction, arm/run/stop lifecycle, cross-thread post, timer.
// No nghttp2 dependency — h2_event_loop.cpp uses only Linux syscall headers.

#include <gtest/gtest.h>
#include "h2_event_loop.hpp"

#include <sys/epoll.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <thread>

namespace {

// ---------------------------------------------------------------------------
// Helper: spin-wait until cond() or timeout elapses.
// ---------------------------------------------------------------------------
template<typename Pred>
static bool wait_for(
    Pred cond,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{2000})
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!cond()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return true;
}

}  // namespace

// ===========================================================================
// Test 1: Constructor does not throw.
// ===========================================================================
TEST(EventLoop, Constructs) {
    EXPECT_NO_THROW({
        h2::EventLoop loop;
        (void)loop;
    });
}

// ===========================================================================
// Test 2: arm()/run()/stop() lifecycle.
// ===========================================================================
TEST(EventLoop, ArmRunStop) {
    h2::EventLoop loop;
    loop.arm();
    EXPECT_TRUE(loop.is_running());

    std::thread t([&loop] { loop.run(); });

    // Give the thread time to enter epoll_wait.
    std::this_thread::sleep_for(std::chrono::milliseconds{20});

    loop.stop();
    t.join();

    EXPECT_FALSE(loop.is_running());
}

// ===========================================================================
// Test 3: stop() before run() — running_ is false, run() exits immediately.
// ===========================================================================
TEST(EventLoop, StopBeforeRunIsClean) {
    h2::EventLoop loop;
    loop.arm();
    loop.stop();   // running_ = false before the run thread starts

    // run() checks while(running_) which is false — should return at once.
    std::thread t([&loop] { loop.run(); });
    t.join();  // should complete within a few ms

    EXPECT_FALSE(loop.is_running());
}

// ===========================================================================
// Test 4: post() from main thread executes callback in the run() thread.
// ===========================================================================
TEST(EventLoop, CrossThreadPost) {
    h2::EventLoop loop;
    std::atomic<bool> executed{false};

    loop.arm();
    std::thread t([&loop] { loop.run(); });

    // Post a lambda that records execution and stops the loop.
    loop.post([&executed, &loop] {
        executed.store(true, std::memory_order_relaxed);
        loop.stop();
    });

    t.join();  // stop() is called inside the post — thread exits cleanly
    EXPECT_TRUE(executed.load());
}

// ===========================================================================
// Test 5: Multiple post() calls — all execute and in-order within a single
//         loop iteration.
// ===========================================================================
TEST(EventLoop, MultiplePostCallsExecute) {
    h2::EventLoop loop;
    std::atomic<int> counter{0};

    loop.arm();
    std::thread t([&loop] { loop.run(); });

    for (int i = 0; i < 5; ++i) {
        loop.post([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }
    // Post the stop *after* the 5 increments.
    loop.post([&loop] { loop.stop(); });

    t.join();
    EXPECT_EQ(counter.load(), 5);
}

// ===========================================================================
// Test 6: add_timer() fires a one-shot callback via the event loop.
//         Timer setup is posted to the loop thread (ownership requirement).
// ===========================================================================
TEST(EventLoop, TimerFiresCallback) {
    h2::EventLoop loop;
    std::atomic<int> fire_count{0};

    loop.arm();
    std::thread t([&loop] { loop.run(); });

    // Schedule timer setup from inside the loop thread via post().
    loop.post([&loop, &fire_count] {
        loop.add_timer(50 /*ms*/, [&loop, &fire_count](uint32_t /*events*/) {
            fire_count.fetch_add(1, std::memory_order_relaxed);
            loop.stop();
        }, /*oneshot=*/true);
    });

    t.join();  // loop exits after timer fires and calls stop()
    EXPECT_EQ(fire_count.load(), 1);
}

// ===========================================================================
// Test 7: add_fd() / remove_fd() roundtrip on a pipe.
// ===========================================================================
TEST(EventLoop, AddAndRemoveFdRoundtrip) {
    h2::EventLoop loop;

    // Create a pipe to use as a dummy fd.
    int pipefd[2] = {-1, -1};
    ASSERT_EQ(::pipe(pipefd), 0);

    // add_fd before run() — safe since the loop is not yet running.
    bool added = loop.add_fd(pipefd[0], EPOLLIN,
                             [](uint32_t /*events*/) {});
    EXPECT_TRUE(added);

    // remove_fd from the same thread (no run() active).
    bool removed = loop.remove_fd(pipefd[0]);
    EXPECT_TRUE(removed);

    ::close(pipefd[0]);
    ::close(pipefd[1]);
}
