// The threaded pipeline must produce exactly what the single-threaded reference produces, on every queue type.

#include <gtest/gtest.h>

#include <cstdint>

#include "queue_types.hpp"

using namespace cclab;

template <class Q>
class PipelineTest : public ::testing::Test {};
TYPED_TEST_SUITE(PipelineTest, PipelineQueues);

namespace {
pipeline::Config small_config(std::uint64_t items) {
  pipeline::Config c;
  c.items = items;
  c.interval_ns = 0;  // as fast as possible
  c.warmup = 0;
  c.start_delay_ns = 0;
  c.queue_capacity = 256;
  return c;
}
}  // namespace

TYPED_TEST(PipelineTest, ThreadedOutputMatchesSingleThreadedReference) {
  const auto cfg = small_config(50'000);
  const auto ref = pipeline::reference(cfg);
  const auto got = pipeline::run<TypeParam>(cfg);
  EXPECT_TRUE(pipeline::same_output(ref, got));
  EXPECT_EQ(got.received, 50'000u);
  EXPECT_EQ(got.order_violations, 0u);
}

TYPED_TEST(PipelineTest, DeterministicAcrossRuns) {
  const auto cfg = small_config(20'000);
  const auto a = pipeline::run<TypeParam>(cfg);
  const auto b = pipeline::run<TypeParam>(cfg);
  EXPECT_TRUE(pipeline::same_output(a, b));
}

TYPED_TEST(PipelineTest, ZeroAndOneItem) {
  const auto zero = pipeline::run<TypeParam>(small_config(0));
  EXPECT_EQ(zero.received, 0u);
  const auto cfg = small_config(1);
  EXPECT_TRUE(pipeline::same_output(pipeline::reference(cfg), pipeline::run<TypeParam>(cfg)));
}

TYPED_TEST(PipelineTest, LatencySamplesAreCollectedAfterWarmup) {
  auto cfg = small_config(5'000);
  cfg.warmup = 1'000;
  const auto got = pipeline::run<TypeParam>(cfg);
  EXPECT_EQ(got.feed_to_strategy.count, 4'000u);
  EXPECT_EQ(got.feed_to_book.count, 4'000u);
}

TEST(PipelineReference, DifferentSeedsGiveDifferentChecksums) {
  auto a = small_config(1'000);
  auto b = small_config(1'000);
  a.seed = 1;
  b.seed = 2;
  EXPECT_FALSE(pipeline::same_output(pipeline::reference(a), pipeline::reference(b)));
}

TEST(PipelineReference, SameSeedSameResult) {
  const auto cfg = small_config(1'000);
  EXPECT_TRUE(pipeline::same_output(pipeline::reference(cfg), pipeline::reference(cfg)));
}

// ---- known-answer tests for the stage logic (hand-computed) ---------------------------------------
// The threaded-vs-reference tests share the Book/Strategy code, so a bug inside it would hit both sides
// equally. These tests pin the expected values independently.

TEST(PipelineStages, BookComputesDeviationFromRunningVwap) {
  pipeline::Book book;
  pipeline::Event e;
  e.price = 100;
  e.qty = 10;  // vwap = 1000/10 = 100 -> deviation 0
  EXPECT_EQ(book.on_tick(e).qty, 0);
  e.price = 110;
  e.qty = 10;  // vwap = 2100/20 = 105 -> deviation 5
  EXPECT_EQ(book.on_tick(e).qty, 5);
  e.price = 90;
  e.qty = 20;  // vwap = 3900/40 = 97 (integer division) -> deviation -7
  EXPECT_EQ(book.on_tick(e).qty, -7);
}

TEST(PipelineStages, StrategyCountsSignalsAndDetectsDisorder) {
  pipeline::Strategy s;
  auto ev = [](std::uint64_t seq, std::int64_t qty) {
    pipeline::Event e;
    e.seq = seq;
    e.price = 100;
    e.qty = qty;
    return e;
  };
  s.on_event(ev(0, 4));   // > 3  -> sell signal
  s.on_event(ev(1, -4));  // < -3 -> buy signal
  s.on_event(ev(2, 3));   // neither
  EXPECT_EQ(s.sell_signals, 1u);
  EXPECT_EQ(s.buy_signals, 1u);
  EXPECT_EQ(s.received, 3u);
  EXPECT_EQ(s.order_violations, 0u);
  s.on_event(ev(5, 0));   // a gap: seq 3 expected
  EXPECT_EQ(s.order_violations, 1u);
}
