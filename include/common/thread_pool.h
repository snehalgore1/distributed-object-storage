#ifndef DOS_COMMON_THREAD_POOL_H_
#define DOS_COMMON_THREAD_POOL_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "common/status.h"

namespace dos {

// Fixed-size worker pool with a bounded task queue (spec Milestone 2).
//
// Design invariants:
//   - Thread count is fixed at construction; never one-thread-per-request.
//   - The queue is bounded; when full, Submit returns kUnavailable rather than
//     growing without limit (backpressure, not OOM).
//   - A task that throws does not tear down its worker; the exception is caught
//     and counted.
//   - Shutdown drains already-queued tasks, then joins all workers cleanly.
class ThreadPool {
public:
  using Task = std::function<void()>;

  // `num_threads` workers, at most `max_queue_size` tasks waiting. Both must be
  // >= 1 (values < 1 are clamped to 1).
  ThreadPool(std::size_t num_threads, std::size_t max_queue_size);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Enqueues a task. Returns kUnavailable if the queue is full, or
  // kUnavailable if the pool is shutting down. Ok means the task is queued.
  Status Submit(Task task);

  // Stops accepting new work, lets workers finish everything already queued,
  // then joins them. Idempotent; also invoked by the destructor.
  void Shutdown();

  std::size_t num_threads() const { return workers_.size(); }
  std::size_t max_queue_size() const { return max_queue_size_; }

  // Observability counters (monotonic).
  uint64_t completed() const { return completed_.load(std::memory_order_relaxed); }
  uint64_t rejected() const { return rejected_.load(std::memory_order_relaxed); }
  uint64_t failed() const { return failed_.load(std::memory_order_relaxed); }

private:
  void WorkerLoop();

  std::mutex mu_;
  std::condition_variable cv_;
  std::queue<Task> queue_;
  bool stopping_ = false;
  const std::size_t max_queue_size_;

  std::vector<std::thread> workers_;

  std::atomic<uint64_t> completed_{0};
  std::atomic<uint64_t> rejected_{0};
  std::atomic<uint64_t> failed_{0};
};

} // namespace dos

#endif // DOS_COMMON_THREAD_POOL_H_
