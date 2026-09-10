#include "common/thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "common/status.h"

namespace dos {
namespace {

using namespace std::chrono_literals;

TEST(ThreadPoolTest, RunsAllSubmittedTasks) {
  ThreadPool pool(4, 1000);
  std::atomic<int> counter{0};
  constexpr int kN = 500;
  for (int i = 0; i < kN; ++i) {
    ASSERT_TRUE(pool.Submit([&counter] { counter.fetch_add(1); }).ok());
  }
  pool.Shutdown(); // drains queued work before returning
  EXPECT_EQ(counter.load(), kN);
  EXPECT_EQ(pool.completed(), static_cast<uint64_t>(kN));
}

TEST(ThreadPoolTest, ThreadCountIsBounded) {
  constexpr std::size_t kThreads = 3;
  ThreadPool pool(kThreads, 1000);
  EXPECT_EQ(pool.num_threads(), kThreads);

  std::atomic<int> concurrent{0};
  std::atomic<int> peak{0};
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(pool.Submit([&] {
                      int now = concurrent.fetch_add(1) + 1;
                      int prev = peak.load();
                      while (now > prev && !peak.compare_exchange_weak(prev, now)) {
                      }
                      std::this_thread::sleep_for(1ms);
                      concurrent.fetch_sub(1);
                    })
                    .ok());
  }
  pool.Shutdown();
  // Never more tasks running at once than there are worker threads.
  EXPECT_LE(peak.load(), static_cast<int>(kThreads));
}

TEST(ThreadPoolTest, FullQueueReturnsUnavailable) {
  // One worker, tiny queue. Block the worker so the queue fills up.
  ThreadPool pool(1, 2);
  std::mutex m;
  std::condition_variable cv;
  bool release = false;

  // This task occupies the single worker and blocks until released.
  ASSERT_TRUE(pool.Submit([&] {
                    std::unique_lock<std::mutex> lk(m);
                    cv.wait(lk, [&] { return release; });
                  })
                  .ok());
  // Give the worker a moment to pick up the blocking task.
  std::this_thread::sleep_for(20ms);

  // Queue capacity is 2: these fill it.
  EXPECT_TRUE(pool.Submit([] {}).ok());
  EXPECT_TRUE(pool.Submit([] {}).ok());

  // Next submission must be rejected with a defined overload error.
  Status s = pool.Submit([] {});
  EXPECT_EQ(s.code(), StatusCode::kUnavailable);
  EXPECT_GE(pool.rejected(), 1u);

  {
    std::lock_guard<std::mutex> lk(m);
    release = true;
  }
  cv.notify_all();
  pool.Shutdown();
}

TEST(ThreadPoolTest, TaskExceptionDoesNotKillWorker) {
  ThreadPool pool(2, 100);
  ASSERT_TRUE(pool.Submit([] { throw std::runtime_error("boom"); }).ok());

  std::atomic<bool> ran_after{false};
  ASSERT_TRUE(pool.Submit([&] { ran_after.store(true); }).ok());
  pool.Shutdown();

  EXPECT_TRUE(ran_after.load());
  EXPECT_GE(pool.failed(), 1u);
}

TEST(ThreadPoolTest, SubmitAfterShutdownIsRejected) {
  ThreadPool pool(2, 100);
  pool.Shutdown();
  Status s = pool.Submit([] {});
  EXPECT_EQ(s.code(), StatusCode::kUnavailable);
}

TEST(ThreadPoolTest, ShutdownIsIdempotent) {
  ThreadPool pool(2, 100);
  pool.Shutdown();
  pool.Shutdown(); // must not hang or crash
  SUCCEED();
}

} // namespace
} // namespace dos
