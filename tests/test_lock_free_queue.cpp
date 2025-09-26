#include "structures/lock_free_queue.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <iostream>

using namespace hft;

void test_basic_operations() {
    std::cout << "Testing basic queue operations..." << std::endl;
    
    LockFreeQueue<int*, 1024> queue;
    int data[10];
    
    // Test empty queue
    assert(queue.empty());
    assert(queue.size() == 0);
    assert(queue.try_dequeue() == nullptr);
    
    // Test enqueue/dequeue
    for (int i = 0; i < 10; ++i) {
        assert(queue.try_enqueue(&data[i]));
        assert(queue.size() == static_cast<size_t>(i + 1));
    }
    
    assert(!queue.empty());
    
    for (int i = 0; i < 10; ++i) {
        int* result = queue.try_dequeue();
        assert(result == &data[i]);
        assert(queue.size() == static_cast<size_t>(9 - i));
    }
    
    assert(queue.empty());
    std::cout << "Basic operations: PASSED" << std::endl;
}

void test_single_producer_single_consumer() {
    std::cout << "Testing SPSC queue..." << std::endl;
    
    SPSCLockFreeQueue<int, 1024> queue;
    constexpr int NUM_ITEMS = 10000;
    
    std::atomic<bool> producer_done{false};
    std::atomic<int> items_consumed{0};
    
    // Producer thread
    std::thread producer([&]() {
        for (int i = 0; i < NUM_ITEMS; ++i) {
            while (!queue.try_enqueue(i)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true);
    });
    
    // Consumer thread
    std::thread consumer([&]() {
        int expected = 0;
        while (expected < NUM_ITEMS) {
            int value;
            if (queue.try_dequeue(value)) {
                assert(value == expected);
                ++expected;
                items_consumed.fetch_add(1);
            } else {
                std::this_thread::yield();
            }
        }
    });
    
    producer.join();
    consumer.join();
    
    assert(items_consumed.load() == NUM_ITEMS);
    std::cout << "SPSC queue: PASSED" << std::endl;
}

void test_multiple_producers_single_consumer() {
    std::cout << "Testing MPSC scenario..." << std::endl;
    
    LockFreeQueue<int*, 4096> queue;
    constexpr int NUM_PRODUCERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 1000;
    constexpr int TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;
    
    std::vector<int> data(TOTAL_ITEMS);
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
        data[i] = i;
    }
    
    std::atomic<int> items_consumed{0};
    std::atomic<bool> producers_done{false};
    
    // Multiple producer threads
    std::vector<std::thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&, p]() {
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                int index = p * ITEMS_PER_PRODUCER + i;
                while (!queue.try_enqueue(&data[index])) {
                    std::this_thread::yield();
                }
            }
        });
    }
    
    // Single consumer thread
    std::thread consumer([&]() {
        while (items_consumed.load() < TOTAL_ITEMS) {
            int* result = queue.try_dequeue();
            if (result) {
                items_consumed.fetch_add(1);
            } else {
                std::this_thread::yield();
            }
        }
    });
    
    for (auto& producer : producers) {
        producer.join();
    }
    
    producers_done.store(true);
    consumer.join();
    
    assert(items_consumed.load() == TOTAL_ITEMS);
    std::cout << "MPSC scenario: PASSED" << std::endl;
}

void test_queue_full_condition() {
    std::cout << "Testing queue full condition..." << std::endl;
    
    LockFreeQueue<int*, 8> small_queue; // Small queue for testing
    int data[10];
    
    // Fill the queue
    int filled = 0;
    for (int i = 0; i < 10; ++i) {
        if (small_queue.try_enqueue(&data[i])) {
            filled++;
        } else {
            break;
        }
    }
    
    // Queue should be full (capacity - 1 due to implementation)
    assert(filled == 7); // One slot reserved for head/tail distinction
    assert(small_queue.full());
    
    // Should not be able to enqueue more
    assert(!small_queue.try_enqueue(&data[9]));
    
    // Dequeue one item
    int* result = small_queue.try_dequeue();
    assert(result == &data[0]);
    assert(!small_queue.full());
    
    // Should be able to enqueue again
    assert(small_queue.try_enqueue(&data[9]));
    
    std::cout << "Queue full condition: PASSED" << std::endl;
}

int main() {
    std::cout << "Lock-Free Queue Tests" << std::endl;
    std::cout << "=====================" << std::endl;
    
    test_basic_operations();
    test_single_producer_single_consumer();
    test_multiple_producers_single_consumer();
    test_queue_full_condition();
    
    std::cout << "\nAll tests PASSED!" << std::endl;
    return 0;
}
