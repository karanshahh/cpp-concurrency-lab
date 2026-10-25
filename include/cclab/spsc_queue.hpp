#pragma once
// spsc_queue.hpp - single-producer / single-consumer lock-free ring buffers.
//
//   SpscQueueSeqCst<T>          every atomic uses the default memory_order_seq_cst
//   SpscQueueAcqRel<T, Padded>  the weakest correct orders: relaxed / acquire / release.
//                               Padded=false puts head and tail on ONE cache line (false-sharing experiment)
//   SpscQueueCached<T>          acquire/release, and each side keeps a private cached copy of the other
//                               side's index so the shared index is read only when really needed
//
// Rules (NOT enforced by the type system): exactly ONE thread may call try_push and exactly ONE thread may
// call try_pop. Capacity must be a power of two. T must be default constructible and copy-assignable.
//
// Design (see docs/MEMORY_ORDERS.md):
//  * head_ and tail_ are free-running counters, never reduced modulo the capacity by hand.
//    slot = index & mask_. They wrap at 2^64; unsigned subtraction (tail - head) stays correct.
//  * Full vs empty: tail - head == capacity -> full; tail == head -> empty. No slot is sacrificed.
//  * tail_ is written only by the producer, head_ only by the consumer.

#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "cclab/platform.hpp"

namespace cclab {

namespace detail {

inline bool is_pow2(std::size_t n) noexcept { return n != 0 && (n & (n - 1)) == 0; }

inline void require_pow2(std::size_t n) {
  if (!is_pow2(n)) throw std::invalid_argument("SPSC queue: capacity must be a power of two");
}

// One atomic index, optionally alone on its own cache line.
template <bool Padded>
struct IndexCell;

template <>
struct IndexCell<true> {
  alignas(kCacheLine) std::atomic<std::size_t> v{0};  // sizeof == kCacheLine: nothing else shares the line
};

template <>
struct IndexCell<false> {
  std::atomic<std::size_t> v{0};
};

}  // namespace detail

// -----------------------------------------------------------------------------------------------
// seq_cst version: correct and the easiest to reason about, but stronger than this algorithm needs.
// -----------------------------------------------------------------------------------------------
template <class T>
class SpscQueueSeqCst {
 public:
  explicit SpscQueueSeqCst(std::size_t capacity) : mask_(capacity - 1), buf_(capacity) {
    detail::require_pow2(capacity);
  }

  bool try_push(const T& v) {
    const std::size_t t = tail_.v.load();  // seq_cst
    const std::size_t h = head_.v.load();  // seq_cst
    if (t - h == buf_.size()) return false;  // full
    buf_[t & mask_] = v;
    tail_.v.store(t + 1);  // seq_cst
    return true;
  }

  bool try_pop(T& out) {
    const std::size_t h = head_.v.load();
    const std::size_t t = tail_.v.load();
    if (h == t) return false;  // empty
    out = buf_[h & mask_];
    head_.v.store(h + 1);
    return true;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }

 private:
  const std::size_t mask_;
  std::vector<T> buf_;
  detail::IndexCell<true> head_;  // consumer-owned
  detail::IndexCell<true> tail_;  // producer-owned
};

// -----------------------------------------------------------------------------------------------
// acquire/release version: the weakest orders that are still correct.
// -----------------------------------------------------------------------------------------------
template <class T, bool Padded = true>
class SpscQueueAcqRel {
 public:
  explicit SpscQueueAcqRel(std::size_t capacity) : mask_(capacity - 1), buf_(capacity) {
    detail::require_pow2(capacity);
  }

  bool try_push(const T& v) {
    // Only this thread writes tail_, so reading our own last value needs no ordering.
    const std::size_t t = tail_.v.load(std::memory_order_relaxed);
    // acquire: pairs with the consumer's release store of head_. If we see the new head, we also see that the
    // consumer has FINISHED reading the slot we are about to overwrite.
    const std::size_t h = head_.v.load(std::memory_order_acquire);
    if (t - h == buf_.size()) return false;  // full
    buf_[t & mask_] = v;
    // release: publishes the slot write above. Pairs with the consumer's acquire load of tail_.
    tail_.v.store(t + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) {
    const std::size_t h = head_.v.load(std::memory_order_relaxed);  // we are the only writer of head_
    // acquire: pairs with the producer's release store of tail_. Seeing the new tail means the slot contents
    // written before that store are visible to us.
    const std::size_t t = tail_.v.load(std::memory_order_acquire);
    if (h == t) return false;  // empty
    out = buf_[h & mask_];
    // release: tells the producer we are done reading this slot (it may overwrite it now).
    head_.v.store(h + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }

 private:
  const std::size_t mask_;
  std::vector<T> buf_;
  detail::IndexCell<Padded> head_;  // consumer-owned
  detail::IndexCell<Padded> tail_;  // producer-owned
};

// -----------------------------------------------------------------------------------------------
// acquire/release + cached copy of the other side's index.
//
// In SpscQueueAcqRel every try_push reads head_ and every try_pop reads tail_: a line the OTHER core keeps
// writing, so it bounces between cores. Here the producer keeps head_cache, a PRIVATE plain copy of head_, and
// re-reads the shared head_ only when the cached value says "full". The consumer does the same with tail_cache.
//
// Why a stale cache is safe: head_ and tail_ only ever increase. A stale head_cache is <= the real head_, so it can
// only UNDER-estimate the free space: we may refresh needlessly, but we never overwrite a slot the consumer still
// needs. A stale tail_cache is <= the real tail_, so it can only under-estimate the available items, and every slot
// below it was published by the release store of the value we acquired when we cached it.
//
// Layout: the producer's fields (tail_, head_cache) share one cache line, the consumer's (head_, tail_cache) share
// another. Everything on a line has the same single writer.
// -----------------------------------------------------------------------------------------------
template <class T>
class SpscQueueCached {
 public:
  explicit SpscQueueCached(std::size_t capacity) : mask_(capacity - 1), buf_(capacity) {
    detail::require_pow2(capacity);
  }

  bool try_push(const T& v) {
    const std::size_t t = prod_.tail.load(std::memory_order_relaxed);
    if (t - prod_.head_cache == buf_.size()) {  // looks full according to the stale copy
      prod_.head_cache = cons_.head.load(std::memory_order_acquire);  // refresh (shared line, rare)
      if (t - prod_.head_cache == buf_.size()) return false;           // really full
    }
    buf_[t & mask_] = v;
    prod_.tail.store(t + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) {
    const std::size_t h = cons_.head.load(std::memory_order_relaxed);
    if (h == cons_.tail_cache) {  // looks empty according to the stale copy
      cons_.tail_cache = prod_.tail.load(std::memory_order_acquire);  // refresh (shared line, rare)
      if (h == cons_.tail_cache) return false;                         // really empty
    }
    out = buf_[h & mask_];
    cons_.head.store(h + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }

 private:
  struct alignas(kCacheLine) ProducerSide {
    std::atomic<std::size_t> tail{0};
    std::size_t head_cache = 0;  // producer-private
  };
  struct alignas(kCacheLine) ConsumerSide {
    std::atomic<std::size_t> head{0};
    std::size_t tail_cache = 0;  // consumer-private
  };

  const std::size_t mask_;
  std::vector<T> buf_;
  ProducerSide prod_;
  ConsumerSide cons_;
};

}  // namespace cclab
