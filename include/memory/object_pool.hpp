#pragma once

#include "memory_pool.hpp"
#include <new>

namespace hft {

template<typename T, size_t PoolSize = 10000>
class ObjectPool {
private:
    MemoryPool<sizeof(T), PoolSize> memory_pool_;
    
public:
    ObjectPool() = default;
    
    template<typename... Args>
    FORCE_INLINE T* construct(Args&&... args) noexcept {
        void* ptr = memory_pool_.allocate();
        if (UNLIKELY(!ptr)) {
            return nullptr;
        }
        
        try {
            return new(ptr) T(std::forward<Args>(args)...);
        } catch (...) {
            memory_pool_.deallocate(ptr);
            return nullptr;
        }
    }
    
    FORCE_INLINE void destroy(T* obj) noexcept {
        if (LIKELY(obj)) {
            obj->~T();
            memory_pool_.deallocate(obj);
        }
    }
    
    FORCE_INLINE size_t capacity() const noexcept {
        return PoolSize;
    }
    
    FORCE_INLINE bool owns(T* obj) const noexcept {
        return memory_pool_.owns(obj);
    }
};

// Specialized object pool for POD types (no constructor/destructor calls)
template<typename T, size_t PoolSize = 10000>
class PODObjectPool {
private:
    static_assert(std::is_trivially_constructible_v<T> && 
                  std::is_trivially_destructible_v<T>,
                  "T must be a POD type");
    
    MemoryPool<sizeof(T), PoolSize> memory_pool_;
    
public:
    PODObjectPool() = default;
    
    FORCE_INLINE T* allocate() noexcept {
        return static_cast<T*>(memory_pool_.allocate());
    }
    
    FORCE_INLINE void deallocate(T* obj) noexcept {
        memory_pool_.deallocate(obj);
    }
    
    FORCE_INLINE size_t capacity() const noexcept {
        return PoolSize;
    }
};

} // namespace hft
