#include "core/market_data_engine.hpp"
#include "utils/high_precision_timer.hpp"
#include "utils/cpu_affinity.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <random>

using namespace hft;

class LatencyBenchmark {
private:
    static constexpr size_t NUM_MESSAGES = 1000000;
    static constexpr size_t WARMUP_MESSAGES = 10000;
    
    std::vector<uint64_t> latencies_;
    std::mt19937 rng_;
    
public:
    LatencyBenchmark() : rng_(std::random_device{}()) {
        latencies_.reserve(NUM_MESSAGES);
    }
    
    void benchmark_lock_free_queue() {
        std::cout << "\n=== Lock-Free Queue Benchmark ===" << std::endl;
        
        LockFreeQueue<int*, 65536> queue;
        std::vector<int> data(NUM_MESSAGES);
        
        // Warmup
        for (size_t i = 0; i < WARMUP_MESSAGES; ++i) {
            queue.try_enqueue(&data[i % data.size()]);
            queue.try_dequeue();
        }
        
        latencies_.clear();
        
        // Benchmark enqueue/dequeue operations
        for (size_t i = 0; i < NUM_MESSAGES; ++i) {
            uint64_t start = HighPrecisionTimer::get_tsc();
            
            queue.try_enqueue(&data[i % data.size()]);
            int* result = queue.try_dequeue();
            
            uint64_t end = HighPrecisionTimer::get_tsc();
            
            if (result) {
                latencies_.push_back(HighPrecisionTimer::tsc_to_ns(end - start));
            }
        }
        
        print_latency_stats("Lock-Free Queue");
    }
    
    void benchmark_memory_pool() {
        std::cout << "\n=== Memory Pool Benchmark ===" << std::endl;
        
        MemoryPool<64, 100000> pool;
        std::vector<void*> ptrs;
        ptrs.reserve(NUM_MESSAGES);
        
        latencies_.clear();
        
        // Benchmark allocation
        for (size_t i = 0; i < NUM_MESSAGES; ++i) {
            uint64_t start = HighPrecisionTimer::get_tsc();
            void* ptr = pool.allocate();
            uint64_t end = HighPrecisionTimer::get_tsc();
            
            if (ptr) {
                ptrs.push_back(ptr);
                latencies_.push_back(HighPrecisionTimer::tsc_to_ns(end - start));
            }
        }
        
        print_latency_stats("Memory Pool Allocation");
        
        // Benchmark deallocation
        latencies_.clear();
        for (void* ptr : ptrs) {
            uint64_t start = HighPrecisionTimer::get_tsc();
            pool.deallocate(ptr);
            uint64_t end = HighPrecisionTimer::get_tsc();
            
            latencies_.push_back(HighPrecisionTimer::tsc_to_ns(end - start));
        }
        
        print_latency_stats("Memory Pool Deallocation");
    }
    
    void benchmark_order_book() {
        std::cout << "\n=== Order Book Benchmark ===" << std::endl;
        
        ObjectPool<Order> order_pool;
        OrderBook book(&order_pool);
        
        latencies_.clear();
        
        // Benchmark order additions
        std::uniform_int_distribution<Price> price_dist(100000, 110000);
        std::uniform_int_distribution<Quantity> qty_dist(100, 10000);
        std::uniform_int_distribution<int> side_dist(0, 1);
        
        for (size_t i = 0; i < NUM_MESSAGES; ++i) {
            OrderId order_id = i + 1;
            Price price = price_dist(rng_);
            Quantity quantity = qty_dist(rng_);
            Side side = side_dist(rng_) ? Side::BUY : Side::SELL;
            
            uint64_t start = HighPrecisionTimer::get_tsc();
            bool success = book.add_order(order_id, price, quantity, side);
            uint64_t end = HighPrecisionTimer::get_tsc();
            
            if (success) {
                latencies_.push_back(HighPrecisionTimer::tsc_to_ns(end - start));
            }
        }
        
        print_latency_stats("Order Book Add Order");
        
        // Benchmark best bid/ask access
        latencies_.clear();
        for (size_t i = 0; i < NUM_MESSAGES; ++i) {
            uint64_t start = HighPrecisionTimer::get_tsc();
            
            const PriceLevel* best_bid = book.get_best_bid();
            const PriceLevel* best_ask = book.get_best_ask();
            Price mid = book.get_mid_price();
            
            uint64_t end = HighPrecisionTimer::get_tsc();
            
            // Prevent optimization
            volatile Price dummy = (best_bid ? best_bid->price : 0) + 
                                  (best_ask ? best_ask->price : 0) + mid;
            (void)dummy;
            
            latencies_.push_back(HighPrecisionTimer::tsc_to_ns(end - start));
        }
        
        print_latency_stats("Order Book Best Quote Access");
    }
    
    void benchmark_message_parsing() {
        std::cout << "\n=== Message Parsing Benchmark ===" << std::endl;
        
        MessageParser parser;
        NetworkMessage network_msg;
        ParsedMessage parsed_msg;
        
        // Create a sample ITCH message
        protocol::AddOrderMessage itch_msg;
        itch_msg.header.length = htons(sizeof(itch_msg));
        itch_msg.header.message_type = 'A';
        itch_msg.header.timestamp = htobe64(123456789);
        itch_msg.order_id = htobe64(12345);
        itch_msg.side = 'B';
        itch_msg.shares = htonl(1000);
        std::memcpy(itch_msg.symbol, "AAPL    ", 8);
        itch_msg.price = htobe64(1500000); // $150.00
        
        network_msg.payload_size = sizeof(itch_msg);
        std::memcpy(network_msg.payload, &itch_msg, sizeof(itch_msg));
        network_msg.receive_timestamp = HighPrecisionTimer::get_timestamp();
        
        latencies_.clear();
        
        // Benchmark message parsing
        for (size_t i = 0; i < NUM_MESSAGES; ++i) {
            uint64_t start = HighPrecisionTimer::get_tsc();
            bool success = parser.parse_message(&network_msg, parsed_msg);
            uint64_t end = HighPrecisionTimer::get_tsc();
            
            if (success) {
                latencies_.push_back(HighPrecisionTimer::tsc_to_ns(end - start));
            }
        }
        
        print_latency_stats("Message Parsing");
    }
    
    void benchmark_end_to_end_latency() {
        std::cout << "\n=== End-to-End Latency Benchmark ===" << std::endl;
        
        // Create minimal engine configuration
        MarketDataEngine::Config config;
        config.enable_fpga = false; // Disable FPGA for benchmark
        config.enable_latency_measurement = true;
        
        auto engine = create_hft_engine(config);
        if (!engine->initialize()) {
            std::cout << "Failed to initialize engine" << std::endl;
            return;
        }
        
        // Add test symbol
        engine->add_symbol(1);
        
        latencies_.clear();
        
        // Simulate message processing pipeline
        ObjectPool<NetworkMessage> msg_pool;
        MessageParser parser;
        
        for (size_t i = 0; i < NUM_MESSAGES / 10; ++i) { // Fewer messages for full pipeline
            // Create network message
            auto* network_msg = msg_pool.construct();
            if (!network_msg) continue;
            
            network_msg->receive_timestamp = HighPrecisionTimer::get_timestamp();
            
            // Create sample message
            protocol::AddOrderMessage itch_msg;
            itch_msg.header.length = htons(sizeof(itch_msg));
            itch_msg.header.message_type = 'A';
            itch_msg.header.timestamp = htobe64(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    network_msg->receive_timestamp.time_since_epoch()).count());
            itch_msg.order_id = htobe64(i + 1);
            itch_msg.side = (i % 2) ? 'B' : 'S';
            itch_msg.shares = htonl(1000);
            std::memcpy(itch_msg.symbol, "TEST    ", 8);
            itch_msg.price = htobe64(1000000 + (i % 1000));
            
            network_msg->payload_size = sizeof(itch_msg);
            std::memcpy(network_msg->payload, &itch_msg, sizeof(itch_msg));
            
            uint64_t start = HighPrecisionTimer::get_tsc();
            
            // Parse message
            ParsedMessage parsed;
            if (parser.parse_message(network_msg, parsed)) {
                // Simulate order book update (without actual engine)
                volatile Price dummy = parsed.order.price;
                (void)dummy;
            }
            
            uint64_t end = HighPrecisionTimer::get_tsc();
            latencies_.push_back(HighPrecisionTimer::tsc_to_ns(end - start));
            
            msg_pool.destroy(network_msg);
        }
        
        print_latency_stats("End-to-End Processing");
    }
    
    void run_all_benchmarks() {
        std::cout << "Ultra-Low Latency Market Data Engine - Performance Benchmarks" << std::endl;
        std::cout << "=============================================================" << std::endl;
        
        // Configure for high performance
        configure_hft_thread(1);
        
        benchmark_lock_free_queue();
        benchmark_memory_pool();
        benchmark_order_book();
        benchmark_message_parsing();
        benchmark_end_to_end_latency();
        
        std::cout << "\nBenchmark completed!" << std::endl;
    }

private:
    void print_latency_stats(const std::string& name) {
        if (latencies_.empty()) {
            std::cout << name << ": No data" << std::endl;
            return;
        }
        
        std::sort(latencies_.begin(), latencies_.end());
        
        uint64_t min_latency = latencies_.front();
        uint64_t max_latency = latencies_.back();
        uint64_t avg_latency = 0;
        for (uint64_t latency : latencies_) {
            avg_latency += latency;
        }
        avg_latency /= latencies_.size();
        
        uint64_t p50 = latencies_[latencies_.size() * 50 / 100];
        uint64_t p95 = latencies_[latencies_.size() * 95 / 100];
        uint64_t p99 = latencies_[latencies_.size() * 99 / 100];
        uint64_t p999 = latencies_[latencies_.size() * 999 / 1000];
        
        std::cout << name << " Results:" << std::endl;
        std::cout << "  Samples: " << latencies_.size() << std::endl;
        std::cout << "  Min:     " << min_latency << " ns" << std::endl;
        std::cout << "  Avg:     " << avg_latency << " ns" << std::endl;
        std::cout << "  P50:     " << p50 << " ns" << std::endl;
        std::cout << "  P95:     " << p95 << " ns" << std::endl;
        std::cout << "  P99:     " << p99 << " ns" << std::endl;
        std::cout << "  P99.9:   " << p999 << " ns" << std::endl;
        std::cout << "  Max:     " << max_latency << " ns" << std::endl;
        std::cout << std::endl;
    }
};

int main() {
    LatencyBenchmark benchmark;
    benchmark.run_all_benchmarks();
    return 0;
}
