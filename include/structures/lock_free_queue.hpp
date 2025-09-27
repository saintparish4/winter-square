#pragma once

#include "common/types.hpp"
#include <atomic>
#include <memory>

namespace hft {

// Multi-producer, multi-consumer lock-free queue
template<typename T, size_t Capacity>
class LockFreeQueue {
private:
    static_assert((Capacity & (Capacity -1 )) == 0, "Capacity must be a power of 2");
    static_assert(Capacity >= 2, "Capacity must be at least 2");
    static_assert(std::is_pointer_v<T>, "T must be a pointer type for MPMC queue");

    struct alignas(CACHE_LINE_SIZE) Node {
        std::atomic<T> data{nullptr};
        // Explicit padding to prevent false sharing
        char padding[CACHE_LINE_SIZE - sizeof(std::atomic<T>)]; 
    };

    // Seperate cache lines for head and tail to prevent false sharing
    CACHE_ALIGNED std::atomic<size_t> head_{0};
    CACHE_ALIGNED std::atomic<size_t> tail_{0};
    CACHE_ALIGNED Node buffer_[Capacity];

    static constexpr size_t MASK = Capacity - 1;

public:
    LockFreeQueue() = default;

    // Non-copyable, non-movable for performance
    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;
    LockFreeQueue(LockFreeQueue&&) = delete;
    LockFreeQueue& operator=(LockFreeQueue&&) = delete;

    FORCE_INLINE HOT_PATH bool try_enqueue(T value) noexcept {
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        
        while (true) {
            const size_t next_tail = (current_tail + 1) & MASK;
            
            // Check if queue is full
            if (UNLIKELY(next_tail == head_.load(std::memory_order_acquire))) {
                return false;
            }
            
            // Try to claim the slot
            if (LIKELY(tail_.compare_exchange_weak(current_tail, next_tail,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_relaxed))) {
                // Successfully claimed slot, now store the data
                buffer_[current_tail].data.store(value, std::memory_order_release);
                return true;
            }
            // current_tail was updated by compare_exchange_weak, retry
        }
    }

    FORCE_INLINE HOT_PATH T try_dequeue() noexcept {
        size_t current_head = head_.load(std::memory_order_relaxed);
        
        while (true) {
            // Check if queue is empty
            if (UNLIKELY(current_head == tail_.load(std::memory_order_acquire))) {
                return nullptr;
            }
            
            // Load data before claiming to avoid race condition
            T item = buffer_[current_head].data.load(std::memory_order_acquire);
            if (UNLIKELY(item == nullptr)) {
                // Data not yet available, producer hasn't finished storing
                CPU_PAUSE();
                current_head = head_.load(std::memory_order_relaxed);
                continue;
            }
            
            // Try to claim the slot
            const size_t next_head = (current_head + 1) & MASK;
            if (LIKELY(head_.compare_exchange_weak(current_head, next_head,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_relaxed))) {
                // Successfully claimed slot, clear the slot
                buffer_[current_head].data.store(nullptr, std::memory_order_relaxed);
                return item;
            }
            // current_head was updated by compare_exchange_weak, retry
        }
    }

// Approximate size - may be inaccurate under high contention
FORCE_INLINE size_t approx_size() const noexcept {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_relaxed);
    return (tail - head) & MASK;
}

FORCE_INLINE bool empty() const noexcept {
    return head_.load(std::memory_order_relaxed) == 
           tail_.load(std::memory_order_relaxed);
}

FORCE_INLINE bool full() const noexcept {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_relaxed);
    return ((tail + 1) & MASK) == head;
}

// Blocking enqueue with busy wait
FORCE_INLINE void enqueue_spin(T item) noexcept {
    while (UNLIKELY(!try_enqueue(item))) {
        CPU_PAUSE();
    }
}

// Blocking dequeue with busy wait
FORCE_INLINE T dequeue_spin() noexcept {
    T item;
    while (UNLIKELY((item = try_dequeue()) == nullptr)) {
        CPU_PAUSE();
    }
    return item;
}
};

// Single producer, single consumer - optimized for maximum performance
template<typename T, size_t Capacity>
class SPSCLockFreeQueue {
private:
static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
static_assert(Capacity >= 2, "Capacity must be at least 2");

// Separate head and tail into different cache lines
struct {
    CACHE_ALIGNED std::atomic<size_t> head{0};
    char padding1[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>)];
} consumer_data_;

struct {
    CACHE_ALIGNED std::atomic<size_t> tail{0};
    char padding2[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>)];
} producer_data_;

// Buffer aligned to cache line
CACHE_ALIGNED T buffer_[Capacity];

static constexpr size_t MASK = Capacity - 1;

public:
SPSCLockFreeQueue() = default;

SPSCLockFreeQueue(const SPSCLockFreeQueue&) = delete;
SPSCLockFreeQueue& operator=(const SPSCLockFreeQueue&) = delete;
SPSCLockFreeQueue(SPSCLockFreeQueue&&) = delete;
SPSCLockFreeQueue& operator=(SPSCLockFreeQueue&&) = delete;

// Producer side (single thread only)
FORCE_INLINE HOT_PATH bool try_enqueue(const T& item) noexcept {
    const size_t current_tail = producer_data_.tail.load(std::memory_order_relaxed);
    const size_t next_tail = (current_tail + 1) & MASK;
    
    // Only need acquire for reading head from consumer
    if (UNLIKELY(next_tail == consumer_data_.head.load(std::memory_order_acquire))) {
        return false; // Queue full
    }
    
    buffer_[current_tail] = item;
    
    // Release semantics to make data visible to consumer
    producer_data_.tail.store(next_tail, std::memory_order_release);
    return true;
}

// Consumer side (single thread only)
FORCE_INLINE HOT_PATH bool try_dequeue(T& item) noexcept {
    const size_t current_head = consumer_data_.head.load(std::memory_order_relaxed);
    
    // Only need acquire for reading tail from producer
    if (UNLIKELY(current_head == producer_data_.tail.load(std::memory_order_acquire))) {
        return false; // Queue empty
    }
    
    item = buffer_[current_head];
    
    // Release semantics to make head update visible to producer
    consumer_data_.head.store((current_head + 1) & MASK, std::memory_order_release);
    return true;
}

// Move semantics version for types that support it
template<typename U = T>
FORCE_INLINE HOT_PATH 
typename std::enable_if_t<std::is_move_constructible_v<U>, bool>
try_enqueue(T&& item) noexcept {
    const size_t current_tail = producer_data_.tail.load(std::memory_order_relaxed);
    const size_t next_tail = (current_tail + 1) & MASK;
    
    if (UNLIKELY(next_tail == consumer_data_.head.load(std::memory_order_acquire))) {
        return false;
    }
    
    buffer_[current_tail] = std::move(item);
    producer_data_.tail.store(next_tail, std::memory_order_release);
    return true;
}

FORCE_INLINE size_t size() const noexcept {
    const size_t tail = producer_data_.tail.load(std::memory_order_acquire);
    const size_t head = consumer_data_.head.load(std::memory_order_acquire);
    return (tail - head) & MASK;
}

FORCE_INLINE bool empty() const noexcept {
    return consumer_data_.head.load(std::memory_order_acquire) == 
           producer_data_.tail.load(std::memory_order_acquire);
}

FORCE_INLINE bool full() const noexcept {
    const size_t tail = producer_data_.tail.load(std::memory_order_acquire);
    const size_t head = consumer_data_.head.load(std::memory_order_acquire);
    return ((tail + 1) & MASK) == head;
}

// Blocking operations with busy wait
FORCE_INLINE void enqueue_spin(const T& item) noexcept {
    while (UNLIKELY(!try_enqueue(item))) {
        CPU_PAUSE();
    }
}

FORCE_INLINE void enqueue_spin(T&& item) noexcept {
    while (UNLIKELY(!try_enqueue(std::move(item)))) {
        CPU_PAUSE();
    }
}

FORCE_INLINE T dequeue_spin() noexcept {
    T item;
    while (UNLIKELY(!try_dequeue(item))) {
        CPU_PAUSE();
    }
    return item;
}

// Batch operations for better throughput
template<typename Iterator>
FORCE_INLINE size_t try_enqueue_batch(Iterator begin, Iterator end) noexcept {
    size_t count = 0;
    for (auto it = begin; it != end; ++it) {
        if (!try_enqueue(*it)) {
            break;
        }
        ++count;
    }
    return count;
}

template<typename Iterator>
FORCE_INLINE size_t try_dequeue_batch(Iterator begin, size_t max_count) noexcept {
    size_t count = 0;
    auto it = begin;
    
    for (size_t i = 0; i < max_count; ++i) {
        if (!try_dequeue(*it)) {
            break;
        }
        ++it;
        ++count;
    }
    return count;
}
};

// Type aliases for common use cases
template<size_t Capacity>
using MessageQueue = SPSCLockFreeQueue<void*, Capacity>;

template<size_t Capacity>
using OrderQueue = SPSCLockFreeQueue<uint64_t, Capacity>;

} // namespace hft
