// pipeline_demo.cpp - runs the 3-stage pipeline (see include/cclab/pipeline.hpp) and prints latencies.
//
//   ./pipeline_demo [--items N] [--interval-ns I] [--warmup W] [--pin FEED,BOOK,STRATEGY]
//
// v1 runs the pipeline on BoundedBlockingQueue (mutex + condition variables), the baseline.
// The output is checked against a single-threaded reference run of the same stages.
// Latency = time the stage received the tick - the tick's SCHEDULED feed time (so a stalled feed still
// counts; see README "Method"). Percentiles only, never a mean.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "cclab/bounded_queue.hpp"
#include "cclab/pipeline.hpp"
#include "cclab/platform.hpp"

using namespace cclab;

namespace {

[[noreturn]] void usage() {
  std::fprintf(stderr, "usage: pipeline_demo [--items N] [--interval-ns I] [--warmup W] [--pin FEED,BOOK,STRATEGY]\n");
  std::exit(2);
}

void print_row(const char* name, const LatencyRecorder::Summary& s) {
  std::printf("%-16s %9llu %9llu %10llu %12llu   (%zu samples)\n", name, (unsigned long long)s.p50,
              (unsigned long long)s.p99, (unsigned long long)s.p999, (unsigned long long)s.max, s.count);
}

}  // namespace

int main(int argc, char** argv) {
  pipeline::Config cfg;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 >= argc) usage();
    const std::string k = argv[i];
    if (k == "--pin") {
      if (std::sscanf(argv[i + 1], "%d,%d,%d", &cfg.pin_feed, &cfg.pin_book, &cfg.pin_strategy) != 3) usage();
      continue;
    }
    const unsigned long long v = std::strtoull(argv[i + 1], nullptr, 10);
    if (k == "--items") cfg.items = v;
    else if (k == "--interval-ns") cfg.interval_ns = v;
    else if (k == "--warmup") cfg.warmup = v;
    else usage();
  }

  std::printf("pipeline_demo v1 | compiler: %s\n", __VERSION__);
  if (const char* w = build_warning()) std::printf("WARNING: %s\n", w);
  std::printf("queue=BoundedBlockingQueue(cv) items=%llu interval_ns=%llu warmup=%llu seed=%llu capacity=%zu hw_threads=%u\n",
              (unsigned long long)cfg.items, (unsigned long long)cfg.interval_ns, (unsigned long long)cfg.warmup,
              (unsigned long long)cfg.seed, cfg.queue_capacity, std::thread::hardware_concurrency());
  std::printf("clock resolution: %llu ns\n", (unsigned long long)clock_resolution_ns());
  if (cfg.pin_feed >= 0) std::printf("pinning: feed cpu %d, book cpu %d, strategy cpu %d\n", cfg.pin_feed, cfg.pin_book, cfg.pin_strategy);
  else std::printf("pinning: none\n");

  const pipeline::Result ref = pipeline::reference(cfg);
  const pipeline::Result got = pipeline::run<BoundedBlockingQueue<pipeline::Event>>(cfg);
  if (got.pin_failed) std::printf("WARNING: a pin_this_thread() call failed; threads are NOT pinned\n");

  const bool match = pipeline::same_output(ref, got);
  std::printf("received=%llu order_violations=%llu buy_signals=%llu sell_signals=%llu checksum=%llu\n",
              (unsigned long long)got.received, (unsigned long long)got.order_violations,
              (unsigned long long)got.buy_signals, (unsigned long long)got.sell_signals, (unsigned long long)got.checksum);
  std::printf("matches single-threaded reference: %s\n", match ? "yes" : "NO");
  std::printf("%-16s %9s %9s %10s %12s\n", "stage (ns)", "p50", "p99", "p99.9", "max");
  print_row("feed->book", got.feed_to_book);
  print_row("feed->strategy", got.feed_to_strategy);
  return match ? 0 : 1;
}
