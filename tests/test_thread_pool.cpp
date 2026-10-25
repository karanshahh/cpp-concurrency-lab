#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

#include "cclab/thread_pool.hpp"

using cclab::ThreadPool;

TEST(ThreadPool, ZeroWorkersRejected) {
  EXPECT_THROW(ThreadPool(0), std::invalid_argument);
}

TEST(ThreadPool, ReturnsValuesThroughFutures) {
  ThreadPool pool(4);
  std::vector<std::future<int>> futs;
  for (int i = 0; i < 100; ++i) futs.push_back(pool.submit([i] { return i * i; }));
  for (int i = 0; i < 100; ++i) EXPECT_EQ(futs[i].get(), i * i);
}

TEST(ThreadPool, ForwardsArguments) {
  ThreadPool pool(2);
  auto f = pool.submit([](int a, int b) { return a + b; }, 20, 22);
  EXPECT_EQ(f.get(), 42);
}

TEST(ThreadPool, ExceptionsArriveThroughTheFuture) {
  ThreadPool pool(2);
  auto f = pool.submit([]() -> int { throw std::runtime_error("boom"); });
  EXPECT_THROW(f.get(), std::runtime_error);
  EXPECT_EQ(pool.submit([] { return 7; }).get(), 7);  // the pool is still usable afterwards
}

TEST(ThreadPool, DestructorRunsAllQueuedTasks) {
  std::atomic<int> done{0};
  constexpr int kTasks = 1000;
  {
    ThreadPool pool(3);
    for (int i = 0; i < kTasks; ++i) pool.submit([&done] { ++done; });
  }  // destructor: stop accepting, drain the queue, join
  EXPECT_EQ(done.load(), kTasks);
}

TEST(ThreadPool, SubmitAfterShutdownThrows) {
  ThreadPool pool(2);
  pool.shutdown();
  EXPECT_THROW(pool.submit([] { return 1; }), std::runtime_error);
}

TEST(ThreadPool, ShutdownIsIdempotent) {
  ThreadPool pool(2);
  pool.shutdown();
  pool.shutdown();  // the destructor will call it a third time
  SUCCEED();
}

TEST(ThreadPool, ManyTasksSumMatches) {
  ThreadPool pool(4);
  std::vector<std::future<long long>> futs;
  for (int i = 1; i <= 1000; ++i) futs.push_back(pool.submit([i] { return static_cast<long long>(i); }));
  long long sum = 0;
  for (auto& f : futs) sum += f.get();
  EXPECT_EQ(sum, 1000LL * 1001 / 2);
}

// Deterministic version of "shutdown must not drop queued work": the only worker is blocked behind a gate
// while 100 tasks wait in the queue, and shutdown() is requested BEFORE the gate opens.
TEST(ThreadPool, ShutdownRunsTasksQueuedBehindABlockedWorker) {
  std::promise<void> gate;
  std::shared_future<void> gate_future = gate.get_future().share();
  std::atomic<int> done{0};
  ThreadPool pool(1);
  pool.submit([gate_future] { gate_future.wait(); });
  for (int i = 0; i < 100; ++i) pool.submit([&done] { ++done; });
  std::thread closer([&pool] { pool.shutdown(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let shutdown() set the stopping flag
  gate.set_value();
  closer.join();
  EXPECT_EQ(done.load(), 100);
}
