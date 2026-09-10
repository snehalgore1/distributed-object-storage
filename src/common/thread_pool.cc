#include "common/thread_pool.h"

#include <algorithm>

namespace dos {

ThreadPool::ThreadPool(std::size_t num_threads, std::size_t max_queue_size)
    : max_queue_size_(std::max<std::size_t>(1, max_queue_size)) {
  num_threads = std::max<std::size_t>(1, num_threads);
  workers_.reserve(num_threads);
  for (std::size_t i = 0; i < num_threads; ++i) {
    workers_.emplace_back([this] { WorkerLoop(); });
  }
}

ThreadPool::~ThreadPool() { Shutdown(); }

Status ThreadPool::Submit(Task task) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_) {
      rejected_.fetch_add(1, std::memory_order_relaxed);
      return Status::Unavailable("thread pool is shutting down");
    }
    if (queue_.size() >= max_queue_size_) {
      rejected_.fetch_add(1, std::memory_order_relaxed);
      return Status::Unavailable("task queue is full");
    }
    queue_.push(std::move(task));
  }
  cv_.notify_one();
  return Status::Ok();
}

void ThreadPool::WorkerLoop() {
  for (;;) {
    Task task;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      // Drain remaining tasks even while stopping; exit only once empty.
      if (queue_.empty()) {
        return; // implies stopping_
      }
      task = std::move(queue_.front());
      queue_.pop();
    }
    // Run outside the lock so tasks never block the queue, and isolate faults:
    // one throwing task must not kill the worker or the service.
    try {
      task();
      completed_.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
      failed_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void ThreadPool::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  cv_.notify_all();
  for (std::thread& t : workers_) {
    if (t.joinable()) {
      t.join();
    }
  }
}

} // namespace dos
