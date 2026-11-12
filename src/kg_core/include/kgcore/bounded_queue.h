#pragma once
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace kgcore {
// Thread-safe bounded FIFO queue with drop-oldest semantics.
// Used between the camera capture thread and the render loop.
template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(size_t capacity) : capacity_(capacity ? capacity : 1) {}
  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  // Pushes item; if full, evicts the oldest element first. Returns true if an
  // element was evicted.
  bool Push(T item) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool evicted = false;
    while (queue_.size() >= capacity_) {
      queue_.pop_front();
      evicted = true;
    }
    queue_.push_back(std::move(item));
    return evicted;
  }

  bool TryPop(T& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }
  size_t Capacity() const { return capacity_; }
  bool Empty() const { return Size() == 0; }
  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
  }

 private:
  size_t capacity_;
  std::deque<T> queue_;
  mutable std::mutex mutex_;
};
}  // namespace kgcore
