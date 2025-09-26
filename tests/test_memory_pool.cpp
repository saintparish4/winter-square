#include "memory/memory_pool.hpp"
#include "memory/object_pool.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <iostream>

using namespace hft;

void test_basic_memory_pool() {
    std::cout << "Testing basic memory pool operations..." << std::endl;
    
    MemoryPool<64, 1000> pool;
    
    // Test allocation
    void* ptr1 = pool.allocate();
    assert(ptr1 != nullptr);
    assert(pool.owns(ptr1));
    
    void* ptr2 = pool.allocate();
    assert(ptr2 != nullptr);
    assert(ptr2 != ptr1);
    assert(pool.owns(ptr2));
    
    // Test deallocation
    pool.deallocate(ptr1);
    pool.deallocate(ptr2);
    
    // Test reallocation (should reuse freed memory)
    void* ptr3 = pool.allocate();
    assert(ptr3 != nullptr);
    assert(pool.owns(ptr3));
    
    pool.deallocate(ptr3);
    
    std::cout << "Basic memory pool: PASSED" << std::endl;
}

void test_memory_pool_exhaustion() {
    std::cout << "Testing memory pool exhaustion..." << std::endl;
    
    MemoryPool<32, 10> small_pool;
    std::vector<void*> ptrs;
    
    // Allocate all blocks
    for (int i = 0; i < 10; ++i) {
        void* ptr = small_pool.allocate();
        if (ptr) {
            ptrs.push_back(ptr);
        }
    }
    
    // Pool should be exhausted
    void* exhausted_ptr = small_pool.allocate();
    assert(exhausted_ptr == nullptr);
    
    // Free one block
    small_pool.deallocate(ptrs.back());
    ptrs.pop_back();
    
    // Should be able to allocate again
    void* new_ptr = small_pool.allocate();
    assert(new_ptr != nullptr);
    
    // Clean up
    small_pool.deallocate(new_ptr);
    for (void* ptr : ptrs) {
        small_pool.deallocate(ptr);
    }
    
    std::cout << "Memory pool exhaustion: PASSED" << std::endl;
}

void test_concurrent_memory_pool() {
    std::cout << "Testing concurrent memory pool access..." << std::endl;
    
    MemoryPool<128, 10000> pool;
    constexpr int NUM_THREADS = 4;
    constexpr int ALLOCS_PER_THREAD = 1000;
    
    std::atomic<int> successful_allocs{0};
    std::atomic<int> successful_deallocs{0};
    
    std::vector<std::thread> threads;
    
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&]() {
            std::vector<void*> local_ptrs;
            
            // Allocate
            for (int i = 0; i < ALLOCS_PER_THREAD; ++i) {
                void* ptr = pool.allocate();
                if (ptr) {
                    local_ptrs.push_back(ptr);
                    successful_allocs.fetch_add(1);
                }
            }
            
            // Deallocate
            for (void* ptr : local_ptrs) {
                pool.deallocate(ptr);
                successful_deallocs.fetch_add(1);
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    assert(successful_allocs.load() == successful_deallocs.load());
    std::cout << "Allocated/Deallocated: " << successful_allocs.load() << " blocks" << std::endl;
    
    std::cout << "Concurrent memory pool: PASSED" << std::endl;
}

void test_object_pool() {
    std::cout << "Testing object pool..." << std::endl;
    
    struct TestObject {
        int value;
        double data;
        
        TestObject(int v = 0, double d = 0.0) : value(v), data(d) {}
    };
    
    ObjectPool<TestObject, 100> obj_pool;
    
    // Test construction
    TestObject* obj1 = obj_pool.construct(42, 3.14);
    assert(obj1 != nullptr);
    assert(obj1->value == 42);
    assert(obj1->data == 3.14);
    assert(obj_pool.owns(obj1));
    
    TestObject* obj2 = obj_pool.construct(100, 2.71);
    assert(obj2 != nullptr);
    assert(obj2 != obj1);
    assert(obj2->value == 100);
    assert(obj2->data == 2.71);
    
    // Test destruction
    obj_pool.destroy(obj1);
    obj_pool.destroy(obj2);
    
    // Test reuse
    TestObject* obj3 = obj_pool.construct(999);
    assert(obj3 != nullptr);
    assert(obj3->value == 999);
    
    obj_pool.destroy(obj3);
    
    std::cout << "Object pool: PASSED" << std::endl;
}

void test_pod_object_pool() {
    std::cout << "Testing POD object pool..." << std::endl;
    
    PODObjectPool<int, 1000> pod_pool;
    
    // Test allocation
    int* num1 = pod_pool.allocate();
    assert(num1 != nullptr);
    *num1 = 42;
    
    int* num2 = pod_pool.allocate();
    assert(num2 != nullptr);
    assert(num2 != num1);
    *num2 = 100;
    
    assert(*num1 == 42);
    assert(*num2 == 100);
    
    // Test deallocation
    pod_pool.deallocate(num1);
    pod_pool.deallocate(num2);
    
    // Test reuse
    int* num3 = pod_pool.allocate();
    assert(num3 != nullptr);
    *num3 = 999;
    assert(*num3 == 999);
    
    pod_pool.deallocate(num3);
    
    std::cout << "POD object pool: PASSED" << std::endl;
}

void test_memory_alignment() {
    std::cout << "Testing memory alignment..." << std::endl;
    
    MemoryPool<64, 100> pool;
    
    // Allocate several blocks and check alignment
    for (int i = 0; i < 10; ++i) {
        void* ptr = pool.allocate();
        assert(ptr != nullptr);
        
        // Check alignment (should be at least 8-byte aligned)
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        assert((addr % 8) == 0);
        
        pool.deallocate(ptr);
    }
    
    std::cout << "Memory alignment: PASSED" << std::endl;
}

int main() {
    std::cout << "Memory Pool Tests" << std::endl;
    std::cout << "=================" << std::endl;
    
    test_basic_memory_pool();
    test_memory_pool_exhaustion();
    test_concurrent_memory_pool();
    test_object_pool();
    test_pod_object_pool();
    test_memory_alignment();
    
    std::cout << "\nAll tests PASSED!" << std::endl;
    return 0;
}
