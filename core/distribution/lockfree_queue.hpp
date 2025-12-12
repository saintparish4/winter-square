#pragma once

#include "../types.hpp"
#include <atomic>
#include <memory>

namespace hft {
namespace core {

// Single Producer Single Consumer lock-free queue
// Optimized for minimum latency with cache-line padding
template <typename T, size_t Size = config::DEFAULT_QUEUE_SIZE>
class SPSCQueue {
  static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");

public:
  SPSCQueue() : head_(0), tail_(0) {
    // Allocate with alignment for optimal cache behavior
    buffer_ = std::make_unique<T[]>(Size);
  }

  // Push element (producer side)
  // Returns true if successful, false if queue is full
  bool push(const T &item) noexcept {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t next_head = (head + 1) & (Size - 1);

    if (next_head == tail_.load(std::memory_order_acquire)) {
      return false; // Queue is full
    }

    buffer_[head] = item;
    head_.store(next_head, std::memory_order_release);
    return true;
  }

  // Push with move semantics
  bool push(T &&item) noexcept {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t next_head = (head + 1) & (Size - 1);

    if (next_head == tail_.load(std::memory_order_acquire)) {
      return false;
    }

    buffer_[head] = std::move(item);
    head_.store(next_head, std::memory_order_release);
    return true;
  }

  // Pop element (consumer side)
  // Returns true if successful, false if queue is empty
  bool pop(T &item) noexcept {
    const size_t tail = tail_.load(std::memory_order_relaxed);

    if (tail == head_.load(std::memory_order_acquire)) {
      return false; // Queue is empty
    }

    item = std::move(buffer_[tail]);
    tail_.store((tail + 1) & (Size - 1), std::memory_order_release);
    return true;
  }

  // Check if queue is empty
  bool empty() const noexcept {
    return tail_.load(std::memory_order_acquire) ==
           head_.load(std::memory_order_acquire);
  }

  // Check if queue is full
  bool full() const noexcept {
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t next_head = (head + 1) & (Size - 1);
    return next_head == tail_.load(std::memory_order_acquire);
  }

  // Get approximate size (may be stale)
  size_t size() const noexcept {
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_acquire);
    return (head - tail) & (Size - 1);
  }

  // Get capacity
  constexpr size_t capacity() const noexcept {
    return Size - 1; // One slot reserved for full detection
  }

private:
  // Cache-line aligned atomic indices
  alignas(config::CACHELINE_SIZE) std::atomic<size_t> head_;
  alignas(config::CACHELINE_SIZE) std::atomic<size_t> tail_;

  // Message buffer
  std::unique_ptr<T[]> buffer_;
};

// Multi-Producer Single Consumer queue
// More overhead than SPSC but supports multiple publishers
template <typename T, size_t Size = config::DEFAULT_QUEUE_SIZE>
class MPSCQueue {
  static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");

  struct Node {
    std::atomic<size_t> sequence;
    T data;
  };

public:
  MPSCQueue() : head_(0), tail_(0) {
    nodes_ = std::make_unique<Node[]>(Size);
    for (size_t i = 0; i < Size; ++i) {
      nodes_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  bool push(const T &item) noexcept {
    size_t head;
    while (true) {
      head = head_.load(std::memory_order_relaxed);
      Node &node = nodes_[head & (Size - 1)];
      size_t seq = node.sequence.load(std::memory_order_acquire);

      intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(head);

      if (diff == 0) {
        // Try to claim this slot
        if (head_.compare_exchange_weak(head, head + 1,
                                        std::memory_order_relaxed)) {
          node.data = item;
          node.sequence.store(head + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false; // Queue is full
      }
    }
  }

  bool pop(T &item) noexcept {
    size_t tail = tail_.load(std::memory_order_relaxed);
    Node &node = nodes_[tail & (Size - 1)];
    size_t seq = node.sequence.load(std::memory_order_acquire);

    intptr_t diff =
        static_cast<intptr_t>(seq) - static_cast<intptr_t>(tail + 1);

    if (diff == 0) {
      item = std::move(node.data);
      node.sequence.store(tail + Size, std::memory_order_release);
      tail_.store(tail + 1, std::memory_order_release);
      return true;
    }

    return false; // Queue is empty
  }

  bool empty() const noexcept {
    size_t tail = tail_.load(std::memory_order_acquire);
    Node &node = nodes_[tail & (Size - 1)];
    size_t seq = node.sequence.load(std::memory_order_acquire);
    return static_cast<intptr_t>(seq) - static_cast<intptr_t>(tail + 1) < 0;
  }

private:
  alignas(config::CACHELINE_SIZE) std::atomic<size_t> head_;
  alignas(config::CACHELINE_SIZE) std::atomic<size_t> tail_;
  std::unique_ptr<Node[]> nodes_;
};

} // namespace core
} // namespace hft