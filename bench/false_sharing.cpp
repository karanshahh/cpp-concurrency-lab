// false_sharing.cpp - two threads, each incrementing its OWN counter.
// Layout A: the two counters are adjacent (same cache line)  -> the line ping-pongs between cores.
// Layout B: each counter alone on its own cache line          -> no sharing.
// The work is identical; only the memory layout differs.
//
// Repeated --runs times (default 3); each layout is reported as the per-run times and a RANGE across runs.
//
//   ./false_sharing [--iters N] [--runs R] [--pin CPU_A,CPU_B]
// Build in Release only: the program warns otherwise.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "cclab/platform.hpp"

using namespace cclab;

namespace {

struct Adjacent {
  std::atomic<std::uint64_t> a{0};
  std::atomic<std::uint64_t> b{0};
};

struct Padded {
  alignas(kCacheLine) std::atomic<std::uint64_t> a{0};
  alignas(kCacheLine) std::atomic<std::uint64_t> b{0};
};

int g_pin_a = -1, g_pin_b = -1;
std::atomic<bool> g_pin_failed{false};

template <class Counters>
double run_once_ms(std::uint64_t iters, bool& ok) {
  Counters c;
  std::atomic<bool> go{false};
  auto work = [&](std::atomic<std::uint64_t>& x, int cpu) {
    if (cpu >= 0 && !pin_this_thread(static_cast<unsigned>(cpu))) g_pin_failed.store(true);
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    for (std::uint64_t i = 0; i < iters; ++i) x.fetch_add(1, std::memory_order_relaxed);
  };
  std::thread t1(work, std::ref(c.a), g_pin_a);
  std::thread t2(work, std::ref(c.b), g_pin_b);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));  // let both threads pin and reach the start flag
  const std::uint64_t t0 = now_ns();
  go.store(true, std::memory_order_release);
  t1.join();
  t2.join();
  const std::uint64_t t1ns = now_ns();
  ok = (c.a.load() == iters && c.b.load() == iters);
  return static_cast<double>(t1ns - t0) / 1e6;
}

template <class Counters>
void report(const char* name, std::uint64_t iters, int runs) {
  std::vector<double> ms;
  bool all_ok = true;
  for (int r = 0; r < runs; ++r) {
    bool ok = false;
    ms.push_back(run_once_ms<Counters>(iters, ok));
    all_ok = all_ok && ok;
  }
  const auto [lo, hi] = std::minmax_element(ms.begin(), ms.end());
  std::printf("%-26s runs(ms):", name);
  for (double v : ms) std::printf(" %.1f", v);
  std::printf("   range %.1f-%.1f ms%s\n", *lo, *hi, all_ok ? "" : "  COUNT MISMATCH");
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t iters = 20'000'000;
  int runs = 3;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    if (k == "--pin") {
      if (std::sscanf(argv[i + 1], "%d,%d", &g_pin_a, &g_pin_b) != 2) { std::fprintf(stderr, "bad --pin\n"); return 2; }
      continue;
    }
    const unsigned long long v = std::strtoull(argv[i + 1], nullptr, 10);
    if (k == "--iters") iters = v;
    else if (k == "--runs") runs = static_cast<int>(v);
    else { std::fprintf(stderr, "usage: false_sharing [--iters N] [--runs R] [--pin CPU_A,CPU_B]\n"); return 2; }
  }
  if (runs < 1) runs = 1;
  std::printf("false_sharing v2 | compiler: %s\n", __VERSION__);
  if (const char* w = build_warning()) std::printf("WARNING: %s\n", w);
  std::printf("workload: iters/thread=%llu runs=%d kCacheLine=%zu hw_threads=%u\n", static_cast<unsigned long long>(iters), runs,
              kCacheLine, std::thread::hardware_concurrency());
  std::printf("seed: n/a | clock resolution: %llu ns\n", static_cast<unsigned long long>(clock_resolution_ns()));
  if (g_pin_a >= 0) std::printf("pinning: cpu %d and cpu %d\n", g_pin_a, g_pin_b);
  else std::printf("pinning: none\n");
  std::printf("lower is better\n");
  report<Adjacent>("adjacent (same line)", iters, runs);
  report<Padded>("padded (own line each)", iters, runs);
  if (g_pin_failed.load()) std::printf("WARNING: a pin_this_thread() call failed; results are NOT pinned\n");
  return 0;
}
