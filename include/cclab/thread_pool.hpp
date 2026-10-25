#pragma once
// thread_pool.hpp - fixed-size thread pool.
//   submit(f, args...) -> std::future<R>   an exception thrown by the task arrives through the future
//   ~ThreadPool() / shutdown()             stops accepting work, RUNS every already-queued task, joins
//
// One mutex + one condition variable + a std::deque of tasks. Simple, not lock-free.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace cclab {

class ThreadPool {
 public:
  explicit ThreadPool(std::size_t workers) {
    if (workers == 0) throw std::invalid_argument("ThreadPool: need at least one worker");
    workers_.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  ~ThreadPool() { shutdown(); }

  template <class F, class... Args>
  auto submit(F&& f, Args&&... args)
      -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
    using R = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
    // std::function needs a copyable callable, packaged_task is move-only -> hold it in a shared_ptr.
    auto task = std::make_shared<std::packaged_task<R()>>(
        [fn = std::forward<F>(f), tup = std::make_tuple(std::forward<Args>(args)...)]() mutable -> R {
          return std::apply(std::move(fn), std::move(tup));
        });
    std::future<R> fut = task->get_future();
    {
      std::lock_guard<std::mutex> lk(m_);
      if (stopping_) throw std::runtime_error("ThreadPool: submit() after shutdown");
      tasks_.emplace_back([task] { (*task)(); });
    }
    cv_.notify_one();
    return fut;
  }

  // Idempotent. Not safe to call concurrently from several threads.
  void shutdown() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stopping_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
      if (t.joinable()) t.join();
    }
  }

  [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }

 private:
  void worker_loop() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return stopping_ || !tasks_.empty(); });
        if (tasks_.empty()) return;  // stopping_ and nothing left: exit
        job = std::move(tasks_.front());
        tasks_.pop_front();
      }
      job();  // run outside the lock; a throwing task is captured by its packaged_task
    }
  }

  std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  std::vector<std::thread> workers_;
  bool stopping_ = false;
};

}  // namespace cclab
