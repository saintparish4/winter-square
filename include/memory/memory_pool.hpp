#pragma once

#include "common/types.hpp"
#include <memory>
#include <atomic>
#include <cstdlib>
#include <new>

namespace hft {

// Ultra-fast memory pool with no locks
template<size_t BlockSize, size_t BlockCount>
class MemoryPool {
private:
    struct alignas(CACHE_LINE_SIZE) Block {
        union {
            Block* next;
            alignas(BlockSize) char data[BlockSize];
        };
    };
    
    CACHE_ALIGNED std::atomic<Block*> free_list_;
    CACHE_ALIGNED Block* blocks_;
    CACHE_ALIGNED char* memory_;
    
    static constexpr size_t TOTAL_SIZE = sizeof(Block) * BlockCount;
    
public:
    MemoryPool() {
        // Allocate aligned memory
        memory_ = static_cast<char*>(std::aligned_alloc(CACHE_LINE_SIZE, TOTAL_SIZE));
        if (!memory_) {
            throw std::bad_alloc();
        }
        
        blocks_ = reinterpret_cast<Block*>(memory_);
        
        // Initialize free list
        for (size_t i = 0; i < BlockCount - 1; ++i) {
            blocks_[i].next = &blocks_[i + 1];
        }
        blocks_[BlockCount - 1].next = nullptr;
        
        free_list_.store(&blocks_[0], std::memory_order_relaxed);
    }
    
    ~MemoryPool() {
        if (memory_) {
            std::free(memory_);
        }
    }
    
    // Non-copyable, non-movable
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;
    
    FORCE_INLINE void* allocate() noexcept {
        Block* block = free_list_.load(std::memory_order_acquire);
        
        while (block) {
            Block* next = block->next;
            if (free_list_.compare_exchange_weak(block, next, 
                                                std::memory_order_release,
                                                std::memory_order_acquire)) {
                return block->data;
            }
            // CAS failed, retry with updated block value
        }
        
        return nullptr; // Pool exhausted
    }
    
    FORCE_INLINE void deallocate(void* ptr) noexcept {
        if (UNLIKELY(!ptr)) return;
        
        Block* block = reinterpret_cast<Block*>(ptr);
        Block* head = free_list_.load(std::memory_order_acquire);
        
        do {
            block->next = head;
        } while (!free_list_.compare_exchange_weak(head, block,
                                                  std::memory_order_release,
                                                  std::memory_order_acquire));
    }
    
    FORCE_INLINE size_t block_size() const noexcept { return BlockSize; }
    FORCE_INLINE size_t block_count() const noexcept { return BlockCount; }
    
    // Check if pointer belongs to this pool
    FORCE_INLINE bool owns(void* ptr) const noexcept {
        return ptr >= memory_ && ptr < (memory_ + TOTAL_SIZE);
    }
};

// Huge page memory pool for even better performance
template<size_t BlockSize, size_t BlockCount>
class HugePageMemoryPool {
private:
    static constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024; // 2MB
    static constexpr size_t TOTAL_SIZE = ((sizeof(void*) + BlockSize) * BlockCount + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1);
    
    struct Block {
        Block* next;
        alignas(BlockSize) char data[BlockSize];
    };
    
    CACHE_ALIGNED std::atomic<Block*> free_list_;
    CACHE_ALIGNED void* memory_;
    CACHE_ALIGNED Block* blocks_;

public:
    HugePageMemoryPool();
    ~HugePageMemoryPool();
    
    FORCE_INLINE void* allocate() noexcept;
    FORCE_INLINE void deallocate(void* ptr) noexcept;
    
    HugePageMemoryPool(const HugePageMemoryPool&) = delete;
    HugePageMemoryPool& operator=(const HugePageMemoryPool&) = delete;
};

} // namespace hft
