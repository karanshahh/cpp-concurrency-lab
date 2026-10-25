#pragma once
// latency_recorder.hpp - records raw latency samples, reports exact percentiles.
//
// Single writer: one thread calls record(); read the summary only after that thread has joined.
// Storage is reserved up front, so record() never allocates (it would disturb what we measure).
// Percentiles use the nearest-rank method on the sorted samples (exact, no bucketing error).
// Only min / p50 / p99 / p99.9 / max are reported; a mean hides the tail, so there is none.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cclab {

class LatencyRecorder {
 public:
  struct Summary {
    std::size_t count = 0;
    std::size_t dropped = 0;  // samples that did not fit in the reserved capacity
    std::uint64_t min = 0, p50 = 0, p99 = 0, p999 = 0, max = 0;
  };

  explicit LatencyRecorder(std::size_t capacity) : capacity_(capacity) { samples_.reserve(capacity); }

  void record(std::uint64_t ns) noexcept {
    if (samples_.size() < capacity_) {
      samples_.push_back(ns);
    } else {
      ++dropped_;
    }
  }

  // Sorts the samples in place. Call once, after the writer thread has finished.
  [[nodiscard]] Summary summarize() {
    Summary s;
    s.dropped = dropped_;
    s.count = samples_.size();
    if (samples_.empty()) return s;
    std::sort(samples_.begin(), samples_.end());
    s.min = samples_.front();
    s.max = samples_.back();
    s.p50 = at_permille(500);
    s.p99 = at_permille(990);
    s.p999 = at_permille(999);
    return s;
  }

 private:
  // Nearest-rank percentile with integer math (floating point rounds 99.9% badly).
  std::uint64_t at_permille(std::uint64_t pm) const {
    const std::uint64_t n = samples_.size();
    std::uint64_t rank = (n * pm + 999) / 1000;  // ceil(n * pm / 1000)
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return samples_[static_cast<std::size_t>(rank - 1)];
  }

  std::vector<std::uint64_t> samples_;
  std::size_t capacity_;
  std::size_t dropped_ = 0;
};

}  // namespace cclab
