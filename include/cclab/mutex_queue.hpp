#pragma once
// mutex_queue.hpp - the simplest correct bounded queue: one mutex, a preallocated ring buffer,
// non-blocking try_push / try_pop. This is the "lock + spin" baseline.
// Any number of producers and consumers. T must be default constructible and copy-assignable.

#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace cclab {

template <class T>
class MutexQueue {
 public:
  explicit MutexQueue(std::size_t capacity) : buf_(capacity) {
    if (capacity == 0) throw std::invalid_argument("MutexQueue: capacity must be > 0");
  }

  bool try_push(const T& v) {
    std::lock_guard<std::mutex> lk(m_);
    if (count_ == buf_.size()) return false;  // full
    buf_[(head_ + count_) % buf_.size()] = v;
    ++count_;
    return true;
  }

  bool try_pop(T& out) {
    std::lock_guard<std::mutex> lk(m_);
    if (count_ == 0) return false;  // empty
    out = buf_[head_];
    head_ = (head_ + 1) % buf_.size();
    --count_;
    return true;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }

 private:
  std::mutex m_;
  std::vector<T> buf_;
  std::size_t head_ = 0;   // index of the oldest element
  std::size_t count_ = 0;  // number of elements
};

}  // namespace cclab
