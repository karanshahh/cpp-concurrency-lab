#include <gtest/gtest.h>

#include <cstdint>

#include "cclab/latency_recorder.hpp"

using cclab::LatencyRecorder;

TEST(LatencyRecorder, EmptySummaryIsZero) {
  LatencyRecorder r(10);
  const auto s = r.summarize();
  EXPECT_EQ(s.count, 0u);
  EXPECT_EQ(s.max, 0u);
}

TEST(LatencyRecorder, KnownPercentilesOn1000Samples) {
  LatencyRecorder r(1000);
  for (std::uint64_t v = 1000; v >= 1; --v) r.record(v);  // recorded in reverse: summarize() must sort
  const auto s = r.summarize();
  EXPECT_EQ(s.count, 1000u);
  EXPECT_EQ(s.min, 1u);
  EXPECT_EQ(s.p50, 500u);
  EXPECT_EQ(s.p99, 990u);
  EXPECT_EQ(s.p999, 999u);
  EXPECT_EQ(s.max, 1000u);
}

TEST(LatencyRecorder, NearestRankRoundsUpOn1001Samples) {
  LatencyRecorder r(1001);
  for (std::uint64_t v = 1; v <= 1001; ++v) r.record(v);
  const auto s = r.summarize();
  EXPECT_EQ(s.p50, 501u);   // ceil(1001 * 0.5) = 501
  EXPECT_EQ(s.p99, 991u);   // ceil(1001 * 0.99) = 991
  EXPECT_EQ(s.p999, 1000u); // ceil(1001 * 0.999) = 1000
  EXPECT_EQ(s.max, 1001u);
}

TEST(LatencyRecorder, SingleSample) {
  LatencyRecorder r(4);
  r.record(42);
  const auto s = r.summarize();
  EXPECT_EQ(s.p50, 42u);
  EXPECT_EQ(s.p999, 42u);
}

TEST(LatencyRecorder, OverCapacityIsCountedNotStored) {
  LatencyRecorder r(10);
  for (int i = 0; i < 15; ++i) r.record(5);
  const auto s = r.summarize();
  EXPECT_EQ(s.count, 10u);
  EXPECT_EQ(s.dropped, 5u);
}
