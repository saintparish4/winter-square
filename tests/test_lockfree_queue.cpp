#include "../core/distribution/lockfree_queue.hpp"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>


using namespace hft::core;

// Test 1: Basic push/pop operations
void test_basic_operations() {
  SPSCQueue<int, 16> queue;

  // Test empty queue
  assert(queue.empty());
  assert(queue.size() == 0);

  // Test push
  assert(queue.push(42));
  assert(!queue.empty());
  assert(queue.size() == 1);

  // Test pop
  int value;
  assert(queue.pop(value));
  assert(value == 42);
  assert(queue.empty());

  std::cout << "✓ Basic operations test passed\n";
}

// Test 2: Queue capacity
void test_capacity() {
  SPSCQueue<int, 4> queue; // Size 4 = 3 usable slots

  // Fill queue
  assert(queue.push(1));
  assert(queue.push(2));
  assert(queue.push(3));

  // Should be full
  assert(!queue.push(4));

  // Pop one element
  int value;
  assert(queue.pop(value));

  // Should be able to push again
  assert(queue.push(4));

  std::cout << "✓ Capacity test passed\n";
}

// Test 3: FIFO ordering
void test_fifo_ordering() {
  SPSCQueue<int, 16> queue;

  // Push sequence
  for (int i = 0; i < 10; i++) {
    assert(queue.push(i));
  }

  // Pop and verify order
  for (int i = 0; i < 10; i++) {
    int value;
    assert(queue.pop(value));
    assert(value == i);
  }

  std::cout << "✓ FIFO ordering test passed\n";
}

// Test 4: Thread safety (single producer, single consumer)
void test_thread_safety() {
  constexpr size_t NUM_ITEMS = 100000;
  SPSCQueue<uint64_t, 1024> queue;

  // Producer thread
  std::thread producer([&queue]() {
    for (uint64_t i = 0; i < NUM_ITEMS; i++) {
      while (!queue.push(i)) {
        std::this_thread::yield();
      }
    }
  });

  // Consumer thread
  std::thread consumer([&queue]() {
    uint64_t expected = 0;
    while (expected < NUM_ITEMS) {
      uint64_t value;
      if (queue.pop(value)) {
        assert(value == expected);
        expected++;
      } else {
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  consumer.join();

  assert(queue.empty());

  std::cout << "✓ Thread safety test passed (100k items)\n";
}

// Test 5: Move semantics
void test_move_semantics() {
  struct MoveOnlyType {
    int value;
    MoveOnlyType()
        : value(0) {} // Default constructor required for array allocation
    MoveOnlyType(int v) : value(v) {}
    MoveOnlyType(const MoveOnlyType &) = delete;
    MoveOnlyType(MoveOnlyType &&other) noexcept : value(other.value) {
      other.value = -1;
    }
    MoveOnlyType &operator=(MoveOnlyType &&) noexcept = default;
  };

  SPSCQueue<MoveOnlyType, 16> queue;

  // Push with move
  MoveOnlyType item(42);
  assert(queue.push(std::move(item)));
  assert(item.value == -1); // Moved from

  // Pop with move
  MoveOnlyType result(0);
  assert(queue.pop(result));
  assert(result.value == 42);

  std::cout << "✓ Move semantics test passed\n";
}

// Test 6: Performance (simple throughput test)
void test_performance() {
  constexpr size_t NUM_ITEMS = 1000000;
  SPSCQueue<uint64_t, 65536> queue;

  auto start = std::chrono::high_resolution_clock::now();

  // Producer
  std::thread producer([&queue]() {
    for (uint64_t i = 0; i < NUM_ITEMS; i++) {
      while (!queue.push(i)) {
        std::this_thread::yield();
      }
    }
  });

  // Consumer
  std::thread consumer([&queue]() {
    uint64_t count = 0;
    while (count < NUM_ITEMS) {
      uint64_t value;
      if (queue.pop(value)) {
        count++;
      } else {
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  consumer.join();

  auto end = std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  double throughput = NUM_ITEMS / (duration.count() / 1000.0);

  std::cout << "✓ Performance test passed\n";
  std::cout << "  Throughput: " << throughput / 1000000.0 << " M ops/sec\n";
  std::cout << "  Time: " << duration.count() << " ms\n";
}

// Test 7: MPSC Queue
void test_mpsc_queue() {
  MPSCQueue<int, 1024> queue;

  // Multiple producers
  constexpr int NUM_PRODUCERS = 4;
  constexpr int ITEMS_PER_PRODUCER = 10000;

  std::vector<std::thread> producers;
  for (int i = 0; i < NUM_PRODUCERS; i++) {
    producers.emplace_back([&queue, i]() {
      for (int j = 0; j < ITEMS_PER_PRODUCER; j++) {
        while (!queue.push(i * ITEMS_PER_PRODUCER + j)) {
          std::this_thread::yield();
        }
      }
    });
  }

  // Single consumer
  std::vector<int> received;
  received.reserve(NUM_PRODUCERS * ITEMS_PER_PRODUCER);

  std::thread consumer([&queue, &received]() {
    while (received.size() < NUM_PRODUCERS * ITEMS_PER_PRODUCER) {
      int value;
      if (queue.pop(value)) {
        received.push_back(value);
      } else {
        std::this_thread::yield();
      }
    }
  });

  for (auto &t : producers) {
    t.join();
  }
  consumer.join();

  assert(received.size() == NUM_PRODUCERS * ITEMS_PER_PRODUCER);

  std::cout << "✓ MPSC queue test passed\n";
}

int main() {
  std::cout << "Running Lock-Free Queue Tests\n";
  std::cout << "==============================\n\n";

  try {
    test_basic_operations();
    test_capacity();
    test_fifo_ordering();
    test_thread_safety();
    test_move_semantics();
    test_performance();
    test_mpsc_queue();

    std::cout << "\n✅ All tests passed!\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "\n❌ Test failed: " << e.what() << "\n";
    return 1;
  }
}