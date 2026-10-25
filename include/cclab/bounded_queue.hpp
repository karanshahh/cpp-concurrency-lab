#pragma once
// bounded_queue.hpp - bounded blocking MPMC queue: one mutex, two condition variables,
// predicate waits, and a close() signal for clean shutdown.
//
// push() blocks while full, pop() blocks while empty.
// close(): wakes everyone. After close, push() returns false; pop() keeps returning the remaining
// items and then std::nullopt ("closed and drained").
// Items live in a preallocated ring (sized once in the constructor), so push/pop never allocate.
// T must be default constructible and move-assignable.

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cclab {

template <class T>
class BoundedBlockingQueue {
 public:
  explicit BoundedBlockingQueue(std::size_t capacity) : buf_(capacity) {
    if (capacity == 0) throw std::invalid_argument("BoundedBlockingQueue: capacity must be > 0");
  }

  // Blocks while full. Returns false (and drops v) if the queue was closed.
  bool push(T v) {
    std::unique_lock<std::mutex> lk(m_);
    // Predicate form of wait() handles spurious wakeups and lost wakeups.
    not_full_.wait(lk, [this] { return closed_ || count_ < buf_.size(); });
    if (closed_) return false;
    buf_[(head_ + count_) % buf_.size()] = std::move(v);
    ++count_;
    lk.unlock();  // notify without holding the lock: the woken thread can run immediately
    not_empty_.notify_one();
    return true;
  }

  // Blocks while empty. Returns nullopt only when the queue is closed AND drained.
  std::optional<T> pop() {
    std::unique_lock<std::mutex> lk(m_);
    not_empty_.wait(lk, [this] { return closed_ || count_ > 0; });
    if (count_ == 0) return std::nullopt;  // closed and drained
    T v = std::move(buf_[head_]);
    head_ = (head_ + 1) % buf_.size();
    --count_;
    lk.unlock();
    not_full_.notify_one();
    return v;
  }

  bool try_push(const T& v) {
    std::unique_lock<std::mutex> lk(m_);
    if (closed_ || count_ >= buf_.size()) return false;
    buf_[(head_ + count_) % buf_.size()] = v;
    ++count_;
    lk.unlock();
    not_empty_.notify_one();
    return true;
  }

  bool try_pop(T& out) {
    std::unique_lock<std::mutex> lk(m_);
    if (count_ == 0) return false;
    out = std::move(buf_[head_]);
    head_ = (head_ + 1) % buf_.size();
    --count_;
    lk.unlock();
    not_full_.notify_one();
    return true;
  }

  void close() {
    {
      std::lock_guard<std::mutex> lk(m_);
      closed_ = true;
    }
    // notify_all: every waiter must re-check its predicate and see closed_.
    not_full_.notify_all();
    not_empty_.notify_all();
  }

  [[nodiscard]] bool closed() const {
    std::lock_guard<std::mutex> lk(m_);
    return closed_;
  }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard<std::mutex> lk(m_);
    return count_;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }

 private:
  mutable std::mutex m_;
  std::condition_variable not_full_;
  std::condition_variable not_empty_;
  std::vector<T> buf_;     // fixed-size ring
  std::size_t head_ = 0;   // index of the oldest element
  std::size_t count_ = 0;  // number of elements
  bool closed_ = false;
};

}  // namespace cclab
