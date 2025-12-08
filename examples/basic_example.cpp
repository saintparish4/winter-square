#include "../core/core_engine.hpp"
#include <iostream>
#include <csignal>
#include <atomic>

using namespace hft::core;

std::atomic<bool> running{true};

void signal_handler(int signal) {
    (void)signal;
    running.store(false);
}

// Simple subscriber that prints messages
class PrintSubscriber : public ISubscriber {
public:
    PrintSubscriber() : message_count_(0) {}
    
    bool on_message(const NormalizedMessage& msg) noexcept override {
        message_count_++;
        
        // Print every 10000th message to avoid flooding
        if (message_count_ % 10000 == 0) {
            std::cout << "Received " << message_count_ << " messages, "
                     << "Latest: seq=" << msg.sequence 
                     << " ts=" << msg.local_timestamp << std::endl;
        }
        
        return true;  // Continue receiving
    }
    
    const char* name() const noexcept override {
        return "PrintSubscriber";
    }
    
    void shutdown() override {
        std::cout << "Total messages received: " << message_count_ << std::endl;
    }
    
private:
    uint64_t message_count_;
};

int main() {
    std::cout << "HFT Core - Basic Example\n";
    std::cout << "========================\n\n";
    
    // Install signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Configure core
    CoreConfig config;
    config.network.interface_ip = "0.0.0.0";
    config.network.multicast_group = "239.1.1.1";
    config.network.port = 10000;
    
    // Create core engine
    CoreEngine engine(config);
    
    // Add subscriber
    engine.add_subscriber(std::make_unique<PrintSubscriber>());
    
    // Initialize
    if (!engine.initialize()) {
        std::cerr << "Failed to initialize core engine\n";
        return 1;
    }
    
    std::cout << "Listening on " << config.network.multicast_group 
              << ":" << config.network.port << "\n";
    std::cout << "Subscribers: " << engine.subscriber_count() << "\n";
    std::cout << "Press Ctrl+C to stop\n\n";
    
    // Start engine
    engine.start();
    
    // Wait for signal
    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // Print stats every second
        auto stats = engine.get_stats();
        std::cout << "Stats: "
                 << "packets=" << stats.packets_received
                 << " parsed=" << stats.messages_parsed
                 << " dispatched=" << stats.messages_dispatched
                 << " dropped=" << stats.packets_dropped
                 << " errors=" << stats.parse_errors
                 << " avg_latency=" << stats.avg_latency_ns() << "ns"
                 << std::endl;
    }
    
    std::cout << "\nStopping...\n";
    engine.stop();
    
    // Final stats
    auto final_stats = engine.get_stats();
    std::cout << "\nFinal Statistics:\n";
    std::cout << "  Packets received: " << final_stats.packets_received << "\n";
    std::cout << "  Messages parsed: " << final_stats.messages_parsed << "\n";
    std::cout << "  Messages dispatched: " << final_stats.messages_dispatched << "\n";
    std::cout << "  Packets dropped: " << final_stats.packets_dropped << "\n";
    std::cout << "  Parse errors: " << final_stats.parse_errors << "\n";
    std::cout << "  Min latency: " << final_stats.min_latency_ns << "ns\n";
    std::cout << "  Max latency: " << final_stats.max_latency_ns << "ns\n";
    std::cout << "  Avg latency: " << final_stats.avg_latency_ns() << "ns\n";
    
    return 0;
}