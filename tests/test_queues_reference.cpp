// Randomized comparison of every queue against a brute-force reference model (a std::deque with a size cap).
// One thread does all operations, so this checks the SEQUENTIAL semantics: FIFO order, full, empty,
// wrap-around. (The threaded behaviour is checked by the stress tests and by TSan.)

#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <random>

#include "queue_types.hpp"

template <class Q>
class QueueReferenceTest : public ::testing::Test {};
TYPED_TEST_SUITE(QueueReferenceTest, AllTryQueues);

TYPED_TEST(QueueReferenceTest, RandomOperationsMatchDequeModel) {
  constexpr std::size_t kCap = 16;  // power of two, so SPSC queues accept it too
  for (std::uint64_t seed = 1; seed <= 30; ++seed) {
    // Three push biases: mostly-empty, balanced, mostly-full. All hit both boundaries.
    const unsigned push_pct = 30 + 20 * static_cast<unsigned>(seed % 3);
    TypeParam q(kCap);
    std::deque<std::uint64_t> model;
    std::mt19937_64 rng(seed);
    std::uint64_t next = 0;
    for (int step = 0; step < 20000; ++step) {
      if (rng() % 100 < push_pct) {
        const bool want = model.size() < kCap;
        const bool got = q.try_push(next);
        ASSERT_EQ(got, want) << "seed " << seed << " step " << step << " (push)";
        if (want) model.push_back(next);
        ++next;
      } else {
        std::uint64_t v = 0xDEADBEEF;
        const bool want = !model.empty();
        const bool got = q.try_pop(v);
        ASSERT_EQ(got, want) << "seed " << seed << " step " << step << " (pop)";
        if (want) {
          ASSERT_EQ(v, model.front()) << "seed " << seed << " step " << step << " (value)";
          model.pop_front();
        }
      }
    }
  }
}

TYPED_TEST(QueueReferenceTest, SameOperationLogGivesSameResultsEveryTime) {
  // Determinism: replaying an identical operation log yields an identical result log.
  auto run_log = [] {
    TypeParam q(8);
    std::mt19937_64 rng(12345);
    std::uint64_t digest = 1469598103934665603ull, next = 0;
    for (int i = 0; i < 5000; ++i) {
      if (rng() & 1) {
        digest = (digest ^ static_cast<std::uint64_t>(q.try_push(next++))) * 1099511628211ull;
      } else {
        std::uint64_t v = 0;
        const bool ok = q.try_pop(v);
        digest = (digest ^ (ok ? v + 1 : 0)) * 1099511628211ull;
      }
    }
    return digest;
  };
  EXPECT_EQ(run_log(), run_log());
}
