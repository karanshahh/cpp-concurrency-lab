#pragma once
// pipeline.hpp - feed thread -> book thread -> strategy thread, joined by two queues.
//
//   feed:      generates synthetic ticks (seeded xorshift), stamps each with its SCHEDULED send time
//   book:      toy book stage: running VWAP, emits each tick's deviation from it
//   strategy:  toy signal stage: counts buy/sell signals, checks sequence order, folds a checksum
//
// A "tick" is one market-data update (price + quantity). VWAP = volume-weighted average price =
// sum(price * qty) / sum(qty). The logic is a TOY on purpose: the point is the cost of the thread
// handoffs, not trading logic.
//
// reference(cfg) computes the same result on ONE thread with the same Book/Strategy/FeedGen code.
// A test proves run<Queue>(cfg) == reference(cfg): the threads and queues add no loss, duplication
// or reordering. The queue type is a template parameter: anything with try_push/try_pop, or with
// blocking push()/pop() (BoundedBlockingQueue).
//
// Shutdown: the feed sends a sentinel tick (seq == kStop); each stage forwards it and exits.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "cclab/latency_recorder.hpp"
#include "cclab/platform.hpp"

namespace cclab::pipeline {

inline constexpr std::uint64_t kStop = ~std::uint64_t{0};

struct Event {
  std::uint64_t seq = 0;
  std::uint64_t t_feed_ns = 0;  // scheduled feed time
  std::int64_t price = 0;
  std::int64_t qty = 0;
};

struct Config {
  std::uint64_t items = 1'000'000;
  std::uint64_t interval_ns = 1000;           // 0 = feed as fast as possible
  std::uint64_t warmup = 10'000;              // first N ticks are not recorded as latency samples
  std::uint64_t seed = 88172645463325252ull;  // must be non-zero (xorshift)
  std::uint64_t start_delay_ns = 20'000'000;  // lets the threads start (and pin) before tick 0
  std::size_t queue_capacity = 4096;          // power of two (SPSC queues require it)
  bool record_latency = true;
  int pin_feed = -1, pin_book = -1, pin_strategy = -1;  // -1 = do not pin
};

struct Result {
  std::uint64_t received = 0, order_violations = 0, buy_signals = 0, sell_signals = 0, checksum = 0;
  LatencyRecorder::Summary feed_to_book, feed_to_strategy;
  bool pin_failed = false;
};

// Compares only the deterministic fields (not latencies).
inline bool same_output(const Result& a, const Result& b) {
  return a.received == b.received && a.order_violations == b.order_violations &&
         a.buy_signals == b.buy_signals && a.sell_signals == b.sell_signals && a.checksum == b.checksum;
}

// ---- stage logic shared by the threaded run and the reference ---------------------------------

class FeedGen {
 public:
  explicit FeedGen(std::uint64_t seed) : x_(seed) {}
  Event next(std::uint64_t seq) {
    price_ += static_cast<std::int64_t>(rnd() % 3) - 1;  // random walk: -1, 0, +1
    Event e;
    e.seq = seq;
    e.price = price_;
    e.qty = 1 + static_cast<std::int64_t>(rnd() % 100);
    return e;
  }

 private:
  std::uint64_t rnd() {  // xorshift64
    x_ ^= x_ << 13;
    x_ ^= x_ >> 7;
    x_ ^= x_ << 17;
    return x_;
  }
  std::uint64_t x_;
  std::int64_t price_ = 10000;
};

class Book {
 public:
  Event on_tick(Event e) {
    vwap_num_ += e.price * e.qty;
    vol_ += e.qty;
    e.qty = e.price - vwap_num_ / vol_;  // deviation from the running VWAP
    return e;
  }

 private:
  std::int64_t vwap_num_ = 0, vol_ = 0;
};

class Strategy {
 public:
  void on_event(const Event& e) {
    if (e.seq != expect_) ++order_violations;
    expect_ = e.seq + 1;
    ++received;
    if (e.qty > 3) ++sell_signals;       // price above running VWAP
    else if (e.qty < -3) ++buy_signals;  // price below running VWAP
    fold(static_cast<std::uint64_t>(e.price));
    fold(static_cast<std::uint64_t>(e.qty));
  }
  std::uint64_t received = 0, order_violations = 0, buy_signals = 0, sell_signals = 0;
  std::uint64_t checksum = 1469598103934665603ull;  // FNV-1a style fold

 private:
  void fold(std::uint64_t v) { checksum = (checksum ^ v) * 1099511628211ull; }
  std::uint64_t expect_ = 0;
};

inline Result to_result(const Strategy& s) {
  Result r;
  r.received = s.received;
  r.order_violations = s.order_violations;
  r.buy_signals = s.buy_signals;
  r.sell_signals = s.sell_signals;
  r.checksum = s.checksum;
  return r;
}

// Single-threaded reference: same stages, no threads, no queues.
inline Result reference(const Config& c) {
  FeedGen gen(c.seed);
  Book book;
  Strategy strat;
  for (std::uint64_t i = 0; i < c.items; ++i) strat.on_event(book.on_tick(gen.next(i)));
  return to_result(strat);
}

// ---- queue adapters: blocking queues use push()/pop(), try_* queues spin ---------------------

template <class Q>
void queue_push(Q& q, const Event& e) {
  if constexpr (requires { q.push(e); }) {
    q.push(e);
  } else {
    while (!q.try_push(e)) cpu_relax();
  }
}

template <class Q>
Event queue_pop(Q& q) {
  if constexpr (requires { q.pop(); }) {
    return *q.pop();  // blocks; the queue is never closed during a run
  } else {
    Event e;
    while (!q.try_pop(e)) cpu_relax();
    return e;
  }
}

// ---- the threaded run -------------------------------------------------------------------------

template <class Q>
Result run(const Config& c) {
  Q q1(c.queue_capacity);  // feed -> book
  Q q2(c.queue_capacity);  // book -> strategy
  const std::size_t cap = c.record_latency ? static_cast<std::size_t>(c.items) : 0;
  LatencyRecorder rec_book(cap);
  LatencyRecorder rec_e2e(cap);
  std::atomic<bool> pin_failed{false};
  auto pin = [&](int cpu) {
    if (cpu >= 0 && !pin_this_thread(static_cast<unsigned>(cpu))) pin_failed.store(true);
  };
  Strategy strat;  // owned by the strategy thread until it is joined
  const std::uint64_t base = now_ns() + c.start_delay_ns;

  std::thread strategy([&] {
    pin(c.pin_strategy);
    for (;;) {
      const Event e = queue_pop(q2);
      if (e.seq == kStop) return;
      const std::uint64_t now = now_ns();
      if (c.record_latency && e.seq >= c.warmup) rec_e2e.record(now > e.t_feed_ns ? now - e.t_feed_ns : 0);
      strat.on_event(e);
    }
  });

  std::thread book_thread([&] {
    pin(c.pin_book);
    Book book;
    for (;;) {
      const Event e = queue_pop(q1);
      if (e.seq == kStop) {
        queue_push(q2, e);  // forward the sentinel, then exit
        return;
      }
      const std::uint64_t now = now_ns();
      if (c.record_latency && e.seq >= c.warmup) rec_book.record(now > e.t_feed_ns ? now - e.t_feed_ns : 0);
      queue_push(q2, book.on_tick(e));
    }
  });

  std::thread feed([&] {
    pin(c.pin_feed);
    FeedGen gen(c.seed);
    for (std::uint64_t i = 0; i < c.items; ++i) {
      const std::uint64_t sched = base + i * c.interval_ns;
      while (now_ns() < sched) cpu_relax();
      Event e = gen.next(i);
      e.t_feed_ns = sched;
      queue_push(q1, e);
    }
    Event stop;
    stop.seq = kStop;
    queue_push(q1, stop);
  });

  feed.join();
  book_thread.join();
  strategy.join();

  Result r = to_result(strat);
  r.feed_to_book = rec_book.summarize();
  r.feed_to_strategy = rec_e2e.summarize();
  r.pin_failed = pin_failed.load();
  return r;
}

}  // namespace cclab::pipeline
