// bench_queues.cpp - throughput and latency of the v1 queues, 1 producer / 1 consumer.
//
// THROUGHPUT: producer pushes --items messages unthrottled; consumer drains them.
//             Mops/s = items / wall time. Message order is verified (a mismatch prints ORDER VIOLATION).
// LATENCY:    producer is PACED: message i is scheduled at base + i*interval and stamped with that
//             SCHEDULED time. If the queue stalls the producer, the delay still counts, because latency
//             is measured from when the message SHOULD have been sent (avoids coordinated omission,
//             Gil Tene, "How NOT to Measure Latency").
//             latency = consumer receive time - scheduled time. The first --warmup messages of each run
//             are discarded. Percentiles only, never a mean.
// Every queue runs the SAME operations. The whole measurement is repeated --runs times (default 3) and
// each cell is reported as a RANGE across runs (lowest-highest), never as the best run.
//
//   ./bench_queues [--items N] [--lat-items N] [--capacity C] [--interval-ns I]
//                  [--runs R] [--warmup W] [--pin PRODUCER_CPU,CONSUMER_CPU]
// Build in Release only: the program warns otherwise.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "cclab/bounded_queue.hpp"
#include "cclab/latency_recorder.hpp"
#include "cclab/mutex_queue.hpp"
#include "cclab/platform.hpp"

using namespace cclab;

namespace {

struct Msg {
  std::uint64_t seq;
  std::uint64_t stamp_ns;
};

struct Args {
  std::size_t items = 2'000'000;
  std::size_t lat_items = 500'000;
  std::size_t capacity = 1024;
  std::size_t warmup = 20'000;
  std::size_t runs = 3;
  std::uint64_t interval_ns = 2000;
  int pin_prod = -1;
  int pin_cons = -1;
};

std::atomic<bool> g_pin_failed{false};

void maybe_pin(int cpu) {
  if (cpu >= 0 && !pin_this_thread(static_cast<unsigned>(cpu))) g_pin_failed.store(true);
}

[[noreturn]] void usage() {
  std::fprintf(stderr,
               "usage: bench_queues [--items N] [--lat-items N] [--capacity C] [--interval-ns I]\n"
               "                    [--runs R] [--warmup W] [--pin PRODUCER_CPU,CONSUMER_CPU]\n");
  std::exit(2);
}

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 >= argc) usage();
    const std::string k = argv[i];
    if (k == "--pin") {
      if (std::sscanf(argv[i + 1], "%d,%d", &a.pin_prod, &a.pin_cons) != 2) usage();
      continue;
    }
    const unsigned long long v = std::strtoull(argv[i + 1], nullptr, 10);
    if (k == "--items") a.items = v;
    else if (k == "--lat-items") a.lat_items = v;
    else if (k == "--capacity") a.capacity = v;
    else if (k == "--interval-ns") a.interval_ns = v;
    else if (k == "--runs") a.runs = v;
    else if (k == "--warmup") a.warmup = v;
    else usage();
  }
  if (a.runs == 0) a.runs = 1;
  if (a.warmup >= a.lat_items) a.warmup = a.lat_items / 10;
  return a;
}

// Adapters give every queue the same push(msg) / pop(msg&) shape.
// MutexQueue only has try_*, so it spins. The cv queue uses its real blocking calls.
template <class Q>
auto spin_push(Q& q) {
  return [&q](const Msg& m) {
    while (!q.try_push(m)) cpu_relax();
  };
}
template <class Q>
auto spin_pop(Q& q) {
  return [&q](Msg& m) {
    while (!q.try_pop(m)) cpu_relax();
  };
}
auto block_push(BoundedBlockingQueue<Msg>& q) {
  return [&q](const Msg& m) { q.push(m); };
}
auto block_pop(BoundedBlockingQueue<Msg>& q) {
  return [&q](Msg& m) {
    auto v = q.pop();  // never nullopt: the queue is not closed during a run
    m = *v;
  };
}

template <class PushFn, class PopFn>
double run_throughput(const Args& a, PushFn push, PopFn pop, bool& ok) {
  const std::size_t n = a.items;
  std::atomic<bool> go{false};
  std::uint64_t bad = 0;  // consumer-owned, read after join
  std::thread cons([&] {
    maybe_pin(a.pin_cons);
    Msg m{};
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    for (std::size_t i = 0; i < n; ++i) {
      pop(m);
      if (m.seq != i) ++bad;
    }
  });
  std::thread prod([&] {
    maybe_pin(a.pin_prod);
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    for (std::size_t i = 0; i < n; ++i) push(Msg{static_cast<std::uint64_t>(i), 0});
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));  // let both threads pin and reach the start flag
  const std::uint64_t t0 = now_ns();
  go.store(true, std::memory_order_release);
  prod.join();
  cons.join();
  const std::uint64_t t1 = now_ns();
  ok = (bad == 0);
  return static_cast<double>(n) / (static_cast<double>(t1 - t0) / 1e9) / 1e6;
}

// Records the post-warm-up samples of one run into `rec` (written by the consumer thread, read after join).
template <class PushFn, class PopFn>
void run_latency(const Args& a, PushFn push, PopFn pop, LatencyRecorder& rec) {
  const std::size_t n = a.lat_items;
  std::atomic<bool> go{false};
  const std::uint64_t base = now_ns() + 20'000'000;  // first message scheduled 20 ms out (threads pin first)
  std::thread cons([&] {
    maybe_pin(a.pin_cons);
    Msg m{};
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    for (std::size_t i = 0; i < n; ++i) {
      pop(m);
      const std::uint64_t now = now_ns();
      if (i >= a.warmup) rec.record(now > m.stamp_ns ? now - m.stamp_ns : 0);
    }
  });
  std::thread prod([&] {
    maybe_pin(a.pin_prod);
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint64_t sched = base + static_cast<std::uint64_t>(i) * a.interval_ns;
      while (now_ns() < sched) cpu_relax();
      push(Msg{static_cast<std::uint64_t>(i), sched});
    }
  });
  go.store(true, std::memory_order_release);
  prod.join();
  cons.join();
}

std::string range_u(const std::vector<std::uint64_t>& v) {
  const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
  char buf[64];
  if (*lo == *hi) std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(*lo));
  else std::snprintf(buf, sizeof buf, "%llu-%llu", static_cast<unsigned long long>(*lo), static_cast<unsigned long long>(*hi));
  return buf;
}

std::string range_d(const std::vector<double>& v) {
  const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.2f-%.2f", *lo, *hi);
  return buf;
}

template <class Q, class MkPush, class MkPop>
void bench(const char* name, const Args& a, MkPush mk_push, MkPop mk_pop) {
  std::vector<double> mops;
  std::vector<std::uint64_t> p50, p99, p999, mx;
  bool all_ok = true;
  for (std::size_t r = 0; r < a.runs; ++r) {
    {
      Q q(a.capacity);
      bool ok = false;
      mops.push_back(run_throughput(a, mk_push(q), mk_pop(q), ok));
      all_ok = all_ok && ok;
    }
    {
      Q q(a.capacity);
      LatencyRecorder rec(a.lat_items - a.warmup);
      run_latency(a, mk_push(q), mk_pop(q), rec);
      const auto s = rec.summarize();
      p50.push_back(s.p50);
      p99.push_back(s.p99);
      p999.push_back(s.p999);
      mx.push_back(s.max);
    }
  }
  std::printf("%-28s %-14s | %-12s %-14s %-16s %-18s%s\n", name, range_d(mops).c_str(), range_u(p50).c_str(),
              range_u(p99).c_str(), range_u(p999).c_str(), range_u(mx).c_str(), all_ok ? "" : "  ORDER VIOLATION");
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  const Args a = parse(argc, argv);
  std::printf("bench_queues v1 | compiler: %s\n", __VERSION__);
  if (const char* w = build_warning()) std::printf("WARNING: %s\n", w);
  std::printf("workload: items=%zu (throughput), lat_items=%zu warmup=%zu interval_ns=%llu (latency), capacity=%zu, runs=%zu\n",
              a.items, a.lat_items, a.warmup, static_cast<unsigned long long>(a.interval_ns), a.capacity, a.runs);
  std::printf("seed: n/a (deterministic schedule) | hw_threads=%u | kCacheLine=%zu\n", std::thread::hardware_concurrency(), kCacheLine);
  std::printf("clock resolution: %llu ns\n", static_cast<unsigned long long>(clock_resolution_ns()));
  if (a.pin_prod >= 0) std::printf("pinning: producer cpu %d, consumer cpu %d\n", a.pin_prod, a.pin_cons);
  else std::printf("pinning: none\n");
  if (std::thread::hardware_concurrency() < 2) std::printf("WARNING: <2 hardware threads, spinning will distort results\n");
  std::printf("every cell = range across %zu runs (lowest-highest); latency in ns\n", a.runs);
  std::printf("%-28s %-14s | %-12s %-14s %-16s %-18s\n", "queue", "Mops/s", "p50", "p99", "p99.9", "max");

  bench<MutexQueue<Msg>>("MutexQueue (lock+spin)", a, [](auto& q) { return spin_push(q); },
                         [](auto& q) { return spin_pop(q); });
  bench<BoundedBlockingQueue<Msg>>("BoundedBlockingQueue (cv)", a, [](auto& q) { return block_push(q); },
                                   [](auto& q) { return block_pop(q); });
  if (g_pin_failed.load()) std::printf("WARNING: a pin_this_thread() call failed; results are NOT pinned\n");
  return 0;
}
