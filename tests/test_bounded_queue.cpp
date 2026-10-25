#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include "cclab/bounded_queue.hpp"
#include "cclab/mutex_queue.hpp"

using cclab::BoundedBlockingQueue;
using cclab::MutexQueue;

TEST(BoundedBlockingQueue, ZeroCapacityRejected) {
  EXPECT_THROW(BoundedBlockingQueue<int>(0), std::invalid_argument);
}

TEST(BoundedBlockingQueue, FifoSingleThread) {
  BoundedBlockingQueue<int> q(4);
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.push(i));
  for (int i = 0; i < 4; ++i) {
    auto v = q.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, i);
  }
}

TEST(BoundedBlockingQueue, TryPushFailsWhenFullTryPopFailsWhenEmpty) {
  BoundedBlockingQueue<int> q(2);
  int out = 0;
  EXPECT_FALSE(q.try_pop(out));
  EXPECT_TRUE(q.try_push(1));
  EXPECT_TRUE(q.try_push(2));
  EXPECT_FALSE(q.try_push(3));
  EXPECT_TRUE(q.try_pop(out));
  EXPECT_EQ(out, 1);
}

TEST(BoundedBlockingQueue, CloseDrainsThenReturnsNullopt) {
  BoundedBlockingQueue<int> q(4);
  q.push(1);
  q.push(2);
  q.close();
  EXPECT_FALSE(q.push(3));  // rejected after close
  EXPECT_EQ(*q.pop(), 1);   // remaining items are still delivered
  EXPECT_EQ(*q.pop(), 2);
  EXPECT_FALSE(q.pop().has_value());  // closed and drained
}

TEST(BoundedBlockingQueue, TryPushRejectedAfterClose) {
  BoundedBlockingQueue<int> q(4);
  q.close();
  EXPECT_FALSE(q.try_push(1));
  EXPECT_TRUE(q.closed());
}

TEST(BoundedBlockingQueue, CloseWakesBlockedConsumer) {
  BoundedBlockingQueue<int> q(4);
  std::atomic<bool> got_nullopt{false};
  std::thread t([&] { got_nullopt = !q.pop().has_value(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  q.close();
  t.join();
  EXPECT_TRUE(got_nullopt.load());
}

TEST(BoundedBlockingQueue, CloseWakesBlockedProducer) {
  BoundedBlockingQueue<int> q(1);
  ASSERT_TRUE(q.push(1));  // now full
  std::atomic<int> second_push{-1};
  std::thread t([&] { second_push = q.push(2) ? 1 : 0; });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  q.close();
  t.join();
  EXPECT_EQ(second_push.load(), 0);
}

TEST(BoundedBlockingQueue, CapacityOneAlternatesPushPop) {
  BoundedBlockingQueue<int> q(1);
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(q.push(i));
    EXPECT_EQ(q.size(), 1u);
    auto v = q.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, i);
    EXPECT_EQ(q.size(), 0u);
  }
}

TEST(BoundedBlockingQueue, RingWrapsAroundManyTimes) {
  BoundedBlockingQueue<int> q(3);  // not a power of two on purpose: the ring uses % arithmetic
  int next_in = 0, next_out = 0;
  for (int round = 0; round < 1000; ++round) {
    for (int i = 0; i < 2; ++i) ASSERT_TRUE(q.push(next_in++));
    for (int i = 0; i < 2; ++i) {
      auto v = q.pop();
      ASSERT_TRUE(v.has_value());
      ASSERT_EQ(*v, next_out++);
    }
  }
}

TEST(BoundedBlockingQueue, MpmcNoLossNoDuplicate) {
  constexpr int kProducers = 4, kConsumers = 4, kPerProducer = 20000;
  BoundedBlockingQueue<int> q(64);
  std::atomic<long long> sum{0};
  std::atomic<int> count{0};

  std::vector<std::thread> consumers;
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&] {
      while (auto v = q.pop()) {
        sum += *v;
        ++count;
      }
    });
  }
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 0; i < kPerProducer; ++i) q.push(p * kPerProducer + i);
    });
  }
  for (auto& t : producers) t.join();
  q.close();
  for (auto& t : consumers) t.join();

  const long long total = static_cast<long long>(kProducers) * kPerProducer;
  EXPECT_EQ(count.load(), total);
  EXPECT_EQ(sum.load(), total * (total - 1) / 2);  // each of 0..total-1 exactly once
}

TEST(MutexQueue, ZeroCapacityRejected) {
  EXPECT_THROW(MutexQueue<int>(0), std::invalid_argument);
}

TEST(MutexQueue, TwoThreadStressNoLossNoDuplicate) {
  constexpr int kN = 100000;
  MutexQueue<int> q(64);
  long long sum = 0;
  bool in_order = true;
  std::thread consumer([&] {
    int v = 0;
    for (int i = 0; i < kN; ++i) {
      while (!q.try_pop(v)) std::this_thread::yield();
      if (v != i) in_order = false;
      sum += v;
    }
  });
  std::thread producer([&] {
    for (int i = 0; i < kN; ++i) {
      while (!q.try_push(i)) std::this_thread::yield();
    }
  });
  producer.join();
  consumer.join();
  EXPECT_TRUE(in_order);
  EXPECT_EQ(sum, static_cast<long long>(kN) * (kN - 1) / 2);
}
