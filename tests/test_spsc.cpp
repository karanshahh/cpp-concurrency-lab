#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <thread>

#include "cclab/spsc_queue.hpp"

using namespace cclab;

using SpscTypes = ::testing::Types<SpscQueueSeqCst<std::uint64_t>, SpscQueueAcqRel<std::uint64_t, true>,
                                   SpscQueueAcqRel<std::uint64_t, false>, SpscQueueCached<std::uint64_t>>;

template <class Q>
class SpscTest : public ::testing::Test {};
TYPED_TEST_SUITE(SpscTest, SpscTypes);

TYPED_TEST(SpscTest, CapacityMustBePowerOfTwo) {
  EXPECT_THROW({ TypeParam q(0); (void)q; }, std::invalid_argument);
  EXPECT_THROW({ TypeParam q(100); (void)q; }, std::invalid_argument);
  TypeParam ok(128);
  EXPECT_EQ(ok.capacity(), 128u);
}

TYPED_TEST(SpscTest, EmptyPopFails) {
  TypeParam q(8);
  std::uint64_t v = 0;
  EXPECT_FALSE(q.try_pop(v));
}

TYPED_TEST(SpscTest, FillsToExactlyCapacityThenFull) {
  TypeParam q(8);
  for (std::uint64_t i = 0; i < 8; ++i) EXPECT_TRUE(q.try_push(i)) << i;  // all 8 slots are usable
  EXPECT_FALSE(q.try_push(99));
  std::uint64_t v = 0;
  EXPECT_TRUE(q.try_pop(v));
  EXPECT_EQ(v, 0u);
  EXPECT_TRUE(q.try_push(99));  // one slot freed
}

TYPED_TEST(SpscTest, CapacityOneWorks) {
  TypeParam q(1);
  std::uint64_t v = 0;
  for (std::uint64_t i = 0; i < 100; ++i) {
    ASSERT_TRUE(q.try_push(i));
    ASSERT_FALSE(q.try_push(i));
    ASSERT_TRUE(q.try_pop(v));
    ASSERT_EQ(v, i);
    ASSERT_FALSE(q.try_pop(v));
  }
}

TYPED_TEST(SpscTest, WrapAroundKeepsFifoOrder) {
  TypeParam q(4);
  std::uint64_t next_in = 0, next_out = 0, v = 0;
  for (int round = 0; round < 1000; ++round) {  // far more pushes than the capacity
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(q.try_push(next_in++));
    for (int i = 0; i < 3; ++i) {
      ASSERT_TRUE(q.try_pop(v));
      ASSERT_EQ(v, next_out++);
    }
  }
}

// Exercises the stale-cache refresh path of SpscQueueCached (harmless for the others): alternate "fill
// completely" and "drain completely" so the cached index is stale at every phase change.
TYPED_TEST(SpscTest, FillDrainCyclesAtFullAndEmptyBoundaries) {
  TypeParam q(8);
  std::uint64_t in = 0, out = 0, v = 0;
  for (int cycle = 0; cycle < 500; ++cycle) {
    for (int i = 0; i < 8; ++i) ASSERT_TRUE(q.try_push(in++));
    ASSERT_FALSE(q.try_push(in));  // full
    for (int i = 0; i < 8; ++i) {
      ASSERT_TRUE(q.try_pop(v));
      ASSERT_EQ(v, out++);
    }
    ASSERT_FALSE(q.try_pop(v));  // empty
  }
}

namespace {
// Two threads, one producer and one consumer; checks order, loss and duplication. TSan checks the memory orders.
template <class Q>
void two_thread_stress(std::size_t capacity, std::uint64_t n, bool& in_order, std::uint64_t& sum) {
  Q q(capacity);
  sum = 0;
  in_order = true;  // consumer-owned until join
  std::thread consumer([&] {
    std::uint64_t v = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
      while (!q.try_pop(v)) std::this_thread::yield();
      if (v != i) in_order = false;
      sum += v;
    }
  });
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < n; ++i) {
      while (!q.try_push(i)) std::this_thread::yield();
    }
  });
  producer.join();
  consumer.join();
}
}  // namespace

TYPED_TEST(SpscTest, TwoThreadStressInOrderNoLossNoDuplicate) {
  constexpr std::uint64_t kN = 300'000;  // sized so the TSan build stays fast
  bool in_order = false;
  std::uint64_t sum = 0;
  two_thread_stress<TypeParam>(1024, kN, in_order, sum);
  EXPECT_TRUE(in_order);
  EXPECT_EQ(sum, kN * (kN - 1) / 2);
}

// A tiny capacity maximises full/empty transitions under contention.
TYPED_TEST(SpscTest, TwoThreadStressTinyCapacity) {
  constexpr std::uint64_t kN = 100'000;
  bool in_order = false;
  std::uint64_t sum = 0;
  two_thread_stress<TypeParam>(2, kN, in_order, sum);
  EXPECT_TRUE(in_order);
  EXPECT_EQ(sum, kN * (kN - 1) / 2);
}
