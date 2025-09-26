#include "memory/memory_pool.hpp"
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

namespace hft {

template<size_t BlockSize, size_t BlockCount>
HugePageMemoryPool<BlockSize, BlockCount>::HugePageMemoryPool() {
    // Try to allocate huge pages for better performance
    memory_ = mmap(nullptr, TOTAL_SIZE, 
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                   -1, 0);
    
    if (memory_ == MAP_FAILED) {
        // Fallback to regular pages
        memory_ = mmap(nullptr, TOTAL_SIZE,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS,
                      -1, 0);
        
        if (memory_ == MAP_FAILED) {
            throw std::bad_alloc();
        }
    }
    
    // Lock pages in memory to prevent swapping
    if (mlock(memory_, TOTAL_SIZE) != 0) {
        // Non-fatal, but performance may be impacted
    }
    
    blocks_ = static_cast<Block*>(memory_);
    
    // Initialize free list
    for (size_t i = 0; i < BlockCount - 1; ++i) {
        blocks_[i].next = &blocks_[i + 1];
    }
    blocks_[BlockCount - 1].next = nullptr;
    
    free_list_.store(&blocks_[0], std::memory_order_relaxed);
}

template<size_t BlockSize, size_t BlockCount>
HugePageMemoryPool<BlockSize, BlockCount>::~HugePageMemoryPool() {
    if (memory_ && memory_ != MAP_FAILED) {
        munlock(memory_, TOTAL_SIZE);
        munmap(memory_, TOTAL_SIZE);
    }
}

template<size_t BlockSize, size_t BlockCount>
void* HugePageMemoryPool<BlockSize, BlockCount>::allocate() noexcept {
    Block* block = free_list_.load(std::memory_order_acquire);
    
    while (block) {
        Block* next = block->next;
        if (free_list_.compare_exchange_weak(block, next,
                                            std::memory_order_release,
                                            std::memory_order_acquire)) {
            return block->data;
        }
    }
    
    return nullptr;
}

template<size_t BlockSize, size_t BlockCount>
void HugePageMemoryPool<BlockSize, BlockCount>::deallocate(void* ptr) noexcept {
    if (!ptr) return;
    
    Block* block = reinterpret_cast<Block*>(ptr);
    Block* head = free_list_.load(std::memory_order_acquire);
    
    do {
        block->next = head;
    } while (!free_list_.compare_exchange_weak(head, block,
                                              std::memory_order_release,
                                              std::memory_order_acquire));
}

// Explicit instantiations for common sizes
template class MemoryPool<64, 10000>;
template class MemoryPool<128, 10000>;
template class MemoryPool<256, 10000>;
template class MemoryPool<512, 10000>;
template class MemoryPool<1024, 10000>;

template class HugePageMemoryPool<64, 100000>;
template class HugePageMemoryPool<128, 100000>;
template class HugePageMemoryPool<256, 100000>;

} // namespace hft
