#pragma once

#include "memory_pool.hpp"
#include <new>
#include <type_traits>
#include <utility>

namespace hft {

template<typename T, size_t PoolSize = 10000>
class ObjectPool {
private:
    static_assert(!std::is_abstract_v<T>, "Cannot pool abstract types");
    static_assert(sizeof(T) > 0, "Cannot pool incomplete types");
    
    MemoryPool<sizeof(T), PoolSize> memory_pool_;
    
public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    static constexpr size_t pool_size = PoolSize;
    
    ObjectPool() = default;
    
    // Non-copyable, non-movable for performance and safety
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;
    
    template<typename... Args>
    FORCE_INLINE HOT_PATH T* construct(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        void* ptr = memory_pool_.allocate();
        if (UNLIKELY(!ptr)) {
            return nullptr;
        }
        
        if constexpr (std::is_nothrow_constructible_v<T, Args...>) {
            // No exception handling needed for nothrow constructible types
            return new(ptr) T(std::forward<Args>(args)...);
        } else {
            // Handle potential constructor exceptions
            try {
                return new(ptr) T(std::forward<Args>(args)...);
            } catch (...) {
                memory_pool_.deallocate(ptr);
                return nullptr;
            }
        }
    }
    
    // Optimized version for default construction
    FORCE_INLINE HOT_PATH T* construct() noexcept(std::is_nothrow_default_constructible_v<T>) {
        void* ptr = memory_pool_.allocate();
        if (UNLIKELY(!ptr)) {
            return nullptr;
        }
        
        if constexpr (std::is_nothrow_default_constructible_v<T>) {
            return new(ptr) T();
        } else {
            try {
                return new(ptr) T();
            } catch (...) {
                memory_pool_.deallocate(ptr);
                return nullptr;
            }
        }
    }
    
    // Copy construction
    FORCE_INLINE T* construct(const T& other) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        void* ptr = memory_pool_.allocate();
        if (UNLIKELY(!ptr)) {
            return nullptr;
        }
        
        if constexpr (std::is_nothrow_copy_constructible_v<T>) {
            return new(ptr) T(other);
        } else {
            try {
                return new(ptr) T(other);
            } catch (...) {
                memory_pool_.deallocate(ptr);
                return nullptr;
            }
        }
    }
    
    // Move construction
    FORCE_INLINE T* construct(T&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        void* ptr = memory_pool_.allocate();
        if (UNLIKELY(!ptr)) {
            return nullptr;
        }
        
        if constexpr (std::is_nothrow_move_constructible_v<T>) {
            return new(ptr) T(std::move(other));
        } else {
            try {
                return new(ptr) T(std::move(other));
            } catch (...) {
                memory_pool_.deallocate(ptr);
                return nullptr;
            }
        }
    }
    
    FORCE_INLINE HOT_PATH void destroy(T* obj) noexcept {
        if (LIKELY(obj)) {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                obj->~T();
            }
            memory_pool_.deallocate(obj);
        }
    }
    
    // Batch operations for better performance
    template<typename... Args>
    FORCE_INLINE size_t construct_batch(T** objects, size_t count, Args&&... args) noexcept {
        size_t constructed = 0;
        
        for (size_t i = 0; i < count; ++i) {
            objects[i] = construct(args...);
            if (UNLIKELY(!objects[i])) {
                break;
            }
            ++constructed;
        }
        
        return constructed;
    }
    
    FORCE_INLINE void destroy_batch(T** objects, size_t count) noexcept {
        for (size_t i = 0; i < count; ++i) {
            destroy(objects[i]);
        }
    }
    
    // Statistics and information
    FORCE_INLINE size_t capacity() const noexcept {
        return memory_pool_.block_count();
    }
    
    FORCE_INLINE size_t allocated_count() const noexcept {
        return memory_pool_.allocated_count();
    }
    
    FORCE_INLINE size_t available_count() const noexcept {
        return memory_pool_.available_count();
    }
    
    FORCE_INLINE bool owns(T* obj) const noexcept {
        return memory_pool_.owns(obj);
    }
    
    FORCE_INLINE bool owns(const T* obj) const noexcept {
        return memory_pool_.owns(const_cast<T*>(obj));
    }
    
    // For debugging and monitoring
    FORCE_INLINE bool empty() const noexcept {
        return allocated_count() == 0;
    }
    
    FORCE_INLINE bool full() const noexcept {
        return available_count() == 0;
    }
    
    // Reset pool (dangerous - only use when you know all objects are destroyed)
    void reset() noexcept {
        memory_pool_.reset();
    }
};

// Specialized object pool for trivial types (POD and trivially constructible/destructible)
template<typename T, size_t PoolSize = 10000>
class TrivialObjectPool {
private:
    static_assert(std::is_trivially_constructible_v<T> && 
                  std::is_trivially_destructible_v<T>,
                  "T must be trivially constructible and destructible");
    static_assert(sizeof(T) > 0, "Cannot pool incomplete types");
    
    MemoryPool<sizeof(T), PoolSize> memory_pool_;
    
public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    static constexpr size_t pool_size = PoolSize;
    
    TrivialObjectPool() = default;
    
    // Non-copyable, non-movable
    TrivialObjectPool(const TrivialObjectPool&) = delete;
    TrivialObjectPool& operator=(const TrivialObjectPool&) = delete;
    TrivialObjectPool(TrivialObjectPool&&) = delete;
    TrivialObjectPool& operator=(TrivialObjectPool&&) = delete;
    
    FORCE_INLINE HOT_PATH T* allocate() noexcept {
        return static_cast<T*>(memory_pool_.allocate());
    }
    
    FORCE_INLINE HOT_PATH void deallocate(T* obj) noexcept {
        memory_pool_.deallocate(obj);
    }
    
    // Zero-initialized allocation
    FORCE_INLINE T* allocate_zero() noexcept {
        T* obj = allocate();
        if (LIKELY(obj)) {
            std::memset(obj, 0, sizeof(T));
        }
        return obj;
    }
    
    // Batch operations
    FORCE_INLINE size_t allocate_batch(T** objects, size_t count) noexcept {
        void** ptrs = reinterpret_cast<void**>(objects);
        return memory_pool_.allocate_batch(ptrs, count);
    }
    
    FORCE_INLINE void deallocate_batch(T** objects, size_t count) noexcept {
        void** ptrs = reinterpret_cast<void**>(objects);
        memory_pool_.deallocate_batch(ptrs, count);
    }
    
    // Statistics
    FORCE_INLINE size_t capacity() const noexcept {
        return memory_pool_.block_count();
    }
    
    FORCE_INLINE size_t allocated_count() const noexcept {
        return memory_pool_.allocated_count();
    }
    
    FORCE_INLINE size_t available_count() const noexcept {
        return memory_pool_.available_count();
    }
    
    FORCE_INLINE bool owns(T* obj) const noexcept {
        return memory_pool_.owns(obj);
    }
    
    FORCE_INLINE bool owns(const T* obj) const noexcept {
        return memory_pool_.owns(const_cast<T*>(obj));
    }
    
    FORCE_INLINE bool empty() const noexcept {
        return allocated_count() == 0;
    }
    
    FORCE_INLINE bool full() const noexcept {
        return available_count() == 0;
    }
    
    void reset() noexcept {
        memory_pool_.reset();
    }
};

// RAII wrapper for automatic object destruction
template<typename T>
class PooledPtr {
private:
    T* ptr_;
    ObjectPool<T>* pool_;
    
public:
    explicit PooledPtr(T* ptr, ObjectPool<T>* pool) noexcept 
        : ptr_(ptr), pool_(pool) {}
    
    ~PooledPtr() noexcept {
        if (ptr_ && pool_) {
            pool_->destroy(ptr_);
        }
    }
    
    // Non-copyable, movable
    PooledPtr(const PooledPtr&) = delete;
    PooledPtr& operator=(const PooledPtr&) = delete;
    
    PooledPtr(PooledPtr&& other) noexcept 
        : ptr_(other.ptr_), pool_(other.pool_) {
        other.ptr_ = nullptr;
        other.pool_ = nullptr;
    }
    
    PooledPtr& operator=(PooledPtr&& other) noexcept {
        if (this != &other) {
            if (ptr_ && pool_) {
                pool_->destroy(ptr_);
            }
            ptr_ = other.ptr_;
            pool_ = other.pool_;
            other.ptr_ = nullptr;
            other.pool_ = nullptr;
        }
        return *this;
    }
    
    T* get() const noexcept { return ptr_; }
    T& operator*() const noexcept { return *ptr_; }
    T* operator->() const noexcept { return ptr_; }
    
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    
    T* release() noexcept {
        T* tmp = ptr_;
        ptr_ = nullptr;
        pool_ = nullptr;
        return tmp;
    }
};

// Helper function to create RAII pooled objects
template<typename T, typename... Args>
FORCE_INLINE PooledPtr<T> make_pooled(ObjectPool<T>& pool, Args&&... args) noexcept {
    T* ptr = pool.construct(std::forward<Args>(args)...);
    return PooledPtr<T>(ptr, &pool);
}

// Convenient type aliases for common HFT types
template<size_t PoolSize = 16384>
using MessagePool = ObjectPool<class Message, PoolSize>;

template<size_t PoolSize = 32768>
using OrderPool = ObjectPool<class Order, PoolSize>;

template<size_t PoolSize = 65536>
using PricePool = TrivialObjectPool<Price, PoolSize>;

template<size_t PoolSize = 65536>
using QuantityPool = TrivialObjectPool<Quantity, PoolSize>;

} // namespace hft