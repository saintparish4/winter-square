#pragma once

#include "common/types.hpp"
#include <memory>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <cassert>

#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#endif 

namespace hft {

// Ultra-fast memory pool with no locks
template<size_t BlockSize, size_t BlockCount>
class MemoryPool {
private:
    static_assert(BlockSize >= sizeof(void*), "BlockSize must be at least sizeof(void*)");
    static_assert(BlockCount > 0, "BlockCount must be greater than 0");
    static_assert((BlockCount & (BlockCount - 1)) == 0, "BlockCount should be power of 2 for optimal performance");

    // Ensure proper alignment for both data and next pointer
    static constexpr size_t ALIGNED_BLOCK_SIZE = (BlockSize + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);

    struct alignas(CACHE_LINE_SIZE) Block {
        union {
            Block* next;
            alignas(ALIGNED_BLOCK_SIZE) char data[ALIGNED_BLOCK_SIZE]; 
        }; 
    };

    CACHE_ALIGNED std::atomic<Block*> free_list_;
    CACHE_ALIGNED Block* blocks_;
    CACHE_ALIGNED char* memory_;
    CACHE_ALIGNED std::atomic<size_t> allocated_count_{0}; // For debugging/monitoring

    static constexpr size_t TOTAL_SIZE = sizeof(Block) * BlockCount;

    // Helper to check if allocation is from this pool (for debugging/monitoring)
    FORCE_INLINE bool is_valid_block(void* ptr) const noexcept {
        if (!ptr) return false;

        const char* char_ptr = static_cast<const char*>(ptr);
        const char* start = reinterpret_cast<const char*>(blocks_);
        const char* end = start + TOTAL_SIZE;

        // Check if pointer is within range and properly aligned
        return char_ptr >= start && char_ptr < end &&
                ((char_ptr - start) % sizeof(Block)) == 0; 
    }

public:
    MemoryPool() : free_list_{nullptr}, blocks_{nullptr}, memory_{nullptr} {
        // Try huge pages first, fallback to regular allocation
        if (!try_allocate_huge_pages()) {
            allocate_regular(); 
        }

        initialize_free_list(); 
    }

    ~MemoryPool() {
        if (memory_) {
#ifdef __linux__
            if (is_huge_page_allocation()) {
                munmap(memory_, TOTAL_SIZE);  
            } else {
                std::free(memory_); 
            }
#else
            std::free(memory_); 
#endif
        }
    }

    // Non-copyable, non-movable
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;

    FORCE_INLINE HOT_PATH void* allocate() noexcept {
        Block* block = free_list_.load(std::memory_order_acquire);

        while (LIKELY(block != nullptr)) {
            Block* next = block->next;

            if (LIKELY(free_list_.compare_exchange_weak(block, next,
                                                       std::memory_order_release,
                                                       std::memory_order_acquire))) {
                // Prefetch the next likely allocation                                        
                if (LIKELY(next != nullptr)) {
                    PREFETCH_READ(next);  
                }

                allocated_count_.fetch_add(1, std::memory_order_relaxed);
                return block->data; 
            }
            // CAS failed, retry with updated block value
            CPU_PAUSE(); 
        }

        return nullptr; // Pool exhausted 
    }

    FORCE_INLINE HOT_PATH void deallocate(void* ptr) noexcept {
        if (UNLIKELY(!ptr)) return;

        // Debug check in debug builds only
        assert(is_valid_block(ptr) && "Pointer does not belong to this pool");

        Block* block = reinterpret_cast<Block*>(
            static_cast<char*>(ptr) - offsetof(Block, data));

        Block* head = free_list_.load(std::memory_order_acquire);

        do {
            block->next = head;
            CPU_PAUSE(); // Reduce Contention 
        } while (UNLIKELY(!free_list_.compare_exchange_weak(head, block,
                                                           std::memory_order_release,
                                                           std::memory_order_acquire)));
        
        allocated_count_.fetch_sub(1, std::memory_order_relaxed); 
    }

     // Batch allocation for better performance
     FORCE_INLINE size_t allocate_batch(void** ptrs, size_t count) noexcept {
        size_t allocated = 0;
        
        for (size_t i = 0; i < count; ++i) {
            ptrs[i] = allocate();
            if (UNLIKELY(!ptrs[i])) {
                break;
            }
            ++allocated;
        }
        
        return allocated;
    }
    
    // Batch deallocation
    FORCE_INLINE void deallocate_batch(void** ptrs, size_t count) noexcept {
        for (size_t i = 0; i < count; ++i) {
            deallocate(ptrs[i]);
        }
    }
    
    // Statistics and information
    FORCE_INLINE size_t block_size() const noexcept { return ALIGNED_BLOCK_SIZE; }
    FORCE_INLINE size_t block_count() const noexcept { return BlockCount; }
    FORCE_INLINE size_t allocated_count() const noexcept { 
        return allocated_count_.load(std::memory_order_relaxed); 
    }
    FORCE_INLINE size_t available_count() const noexcept { 
        return BlockCount - allocated_count(); 
    }
    
    // Check if pointer belongs to this pool
    FORCE_INLINE bool owns(void* ptr) const noexcept {
        if (!ptr) return false;
        return ptr >= memory_ && ptr < (memory_ + TOTAL_SIZE);
    }
    
    // Reset pool (dangerous - only use when you know all allocations are freed)
    void reset() noexcept {
        allocated_count_.store(0, std::memory_order_relaxed);
        initialize_free_list();
    }

private:
    void initialize_free_list() noexcept {
        blocks_ = reinterpret_cast<Block*>(memory_);
        
        // Initialize free list with better cache locality
        for (size_t i = 0; i < BlockCount - 1; ++i) {
            blocks_[i].next = &blocks_[i + 1];
        }
        blocks_[BlockCount - 1].next = nullptr;
        
        free_list_.store(&blocks_[0], std::memory_order_relaxed);
    }
    
    bool try_allocate_huge_pages() noexcept {
#ifdef __linux__
        // Try to allocate using huge pages for better TLB performance
        memory_ = static_cast<char*>(mmap(nullptr, TOTAL_SIZE, 
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                                         -1, 0));
        
        if (memory_ != MAP_FAILED) {
            // Try to lock pages in memory to prevent swapping
            if (mlock(memory_, TOTAL_SIZE) == 0) {
                return true;
            }
            
            // If mlock fails, still use the allocation but warn
            return true;
        }
        
        memory_ = nullptr;
#endif
        return false;
    }
    
    void allocate_regular() {
        // Fallback to regular aligned allocation
        memory_ = static_cast<char*>(std::aligned_alloc(CACHE_LINE_SIZE, TOTAL_SIZE));
        if (!memory_) {
            throw std::bad_alloc();
        }
        
        // Zero the memory for consistent behavior
        std::memset(memory_, 0, TOTAL_SIZE);
    }
    
    bool is_huge_page_allocation() const noexcept {
#ifdef __linux__
        // Simple heuristic: if mmap was used, it was huge pages
        return memory_ && reinterpret_cast<uintptr_t>(memory_) % PAGE_SIZE == 0;
#endif
        return false;
    }
};

// NUMA-aware memory pool for multi-socket systems
template<size_t BlockSize, size_t BlockCount>
class NUMAMemoryPool {
private:
    static constexpr size_t MAX_NUMA_NODES = 8;
    
    std::unique_ptr<MemoryPool<BlockSize, BlockCount/MAX_NUMA_NODES>> pools_[MAX_NUMA_NODES];
    std::atomic<size_t> current_node_{0};
    size_t num_nodes_;
    
public:
    NUMAMemoryPool() {
        // In a real implementation, you'd detect NUMA topology
        // For now, use a simple approach
        num_nodes_ = std::min(size_t(4), MAX_NUMA_NODES); // Assume up to 4 nodes
        
        for (size_t i = 0; i < num_nodes_; ++i) {
            pools_[i] = std::make_unique<MemoryPool<BlockSize, BlockCount/MAX_NUMA_NODES>>();
        }
    }
    
    FORCE_INLINE void* allocate() noexcept {
        // Simple round-robin allocation across NUMA nodes
        size_t node = current_node_.fetch_add(1, std::memory_order_relaxed) % num_nodes_;
        
        void* ptr = pools_[node]->allocate();
        if (LIKELY(ptr)) return ptr;
        
        // Try other nodes if the preferred node is exhausted
        for (size_t i = 1; i < num_nodes_; ++i) {
            size_t try_node = (node + i) % num_nodes_;
            ptr = pools_[try_node]->allocate();
            if (ptr) return ptr;
        }
        
        return nullptr;
    }
    
    FORCE_INLINE void deallocate(void* ptr) noexcept {
        if (!ptr) return;
        
        // Find which pool owns this pointer
        for (size_t i = 0; i < num_nodes_; ++i) {
            if (pools_[i]->owns(ptr)) {
                pools_[i]->deallocate(ptr);
                return;
            }
        }
        
        // Should never reach here in correct usage
        assert(false && "Pointer does not belong to any NUMA pool");
    }
};

// Convenient type aliases for common block sizes
using MessagePool = MemoryPool<1024, 16384>;    // 1KB blocks
using OrderPool = MemoryPool<256, 32768>;       // 256B blocks  
using SmallPool = MemoryPool<64, 65536>;        // 64B blocks
    
} // namespace hft
