#pragma once

#include "common/types.hpp"
#include <atomic>
#include <memory>

namespace hft {

template<typename T, size_t Capacity>
class LockFreeQueue {
private:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    
    struct alignas(CACHE_LINE_SIZE) Node {
        std::atomic<T*> data{nullptr};
        char padding[CACHE_LINE_SIZE - sizeof(std::atomic<T*>)];
    };

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

    FORCE_INLINE bool try_enqueue(T* item) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (tail + 1) & MASK;
        
        if (UNLIKELY(next_tail == head_.load(std::memory_order_acquire))) {
            return false; // Queue full
        }
        
        buffer_[tail].data.store(item, std::memory_order_relaxed);
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    FORCE_INLINE T* try_dequeue() noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        
        if (UNLIKELY(head == tail_.load(std::memory_order_acquire))) {
            return nullptr; // Queue empty
        }
        
        T* item = buffer_[head].data.load(std::memory_order_relaxed);
        head_.store((head + 1) & MASK, std::memory_order_release);
        return item;
    }

    FORCE_INLINE size_t size() const noexcept {
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t head = head_.load(std::memory_order_acquire);
        return (tail - head) & MASK;
    }

    FORCE_INLINE bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == 
               tail_.load(std::memory_order_acquire);
    }

    FORCE_INLINE bool full() const noexcept {
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t head = head_.load(std::memory_order_acquire);
        return ((tail + 1) & MASK) == head;
    }
};

// Specialized version for single producer, single consumer (even faster)
template<typename T, size_t Capacity>
class SPSCLockFreeQueue {
private:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    
    CACHE_ALIGNED std::atomic<size_t> head_{0};
    CACHE_ALIGNED std::atomic<size_t> tail_{0};
    CACHE_ALIGNED T buffer_[Capacity];
    
    static constexpr size_t MASK = Capacity - 1;

public:
    SPSCLockFreeQueue() = default;
    
    SPSCLockFreeQueue(const SPSCLockFreeQueue&) = delete;
    SPSCLockFreeQueue& operator=(const SPSCLockFreeQueue&) = delete;

    FORCE_INLINE bool try_enqueue(const T& item) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (tail + 1) & MASK;
        
        if (UNLIKELY(next_tail == head_.load(std::memory_order_acquire))) {
            return false;
        }
        
        buffer_[tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    FORCE_INLINE bool try_dequeue(T& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        
        if (UNLIKELY(head == tail_.load(std::memory_order_acquire))) {
            return false;
        }
        
        item = buffer_[head];
        head_.store((head + 1) & MASK, std::memory_order_release);
        return true;
    }
};

} // namespace hft
