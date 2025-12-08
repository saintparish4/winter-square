#pragma once

#include "types.hpp"
#include "network/udp_receiver.hpp"
#include "parser/parser_interface.hpp"
#include "distribution/dispatcher.hpp"
#include "distribution/subscriber_interface.hpp"
#include <memory>
#include <thread>
#include <atomic>

namespace hft::core {

// Core engine configuration
struct CoreConfig {
    UDPConfig network;
    int network_thread_cpu{config::NETWORK_THREAD_CPU};
    int dispatcher_thread_cpu{config::DISPATCHER_THREAD_CPU};
    int parser_thread_cpu{-1};  // -1 = no affinity
    size_t max_messages_per_packet{16};  // Max normalized messages per packet
    
    CoreConfig() = default;
};

// Main core engine - orchestrates network, parsing, and distribution
class CoreEngine {
public:
    explicit CoreEngine(const CoreConfig& config = CoreConfig{})
        : config_(config)
        , receiver_(config.network)
        , dispatcher_()
        , parser_(nullptr)
        , running_(false)
        , stats_() 
    {
        // Default to echo parser if none provided
        set_parser(std::make_unique<EchoParser>());
    }
    
    ~CoreEngine() {
        stop();
    }
    
    // Set the protocol parser
    void set_parser(std::unique_ptr<IParser> parser) {
        if (running_.load()) {
            throw std::runtime_error("Cannot change parser while running");
        }
        parser_ = std::move(parser);
    }
    
    // Add a subscriber
    void add_subscriber(std::unique_ptr<ISubscriber> subscriber) {
        if (running_.load()) {
            throw std::runtime_error("Cannot add subscriber while running");
        }
        dispatcher_.add_subscriber(std::move(subscriber));
    }
    
    // Initialize all components
    bool initialize() {
        // Initialize network receiver
        if (!receiver_.initialize()) {
            return false;
        }
        
        // Initialize parser
        if (parser_) {
            parser_->initialize();
        }
        
        return true;
    }
    
    // Start the core engine
    void start() {
        if (running_.load()) return;
        
        if (!parser_) {
            throw std::runtime_error("No parser configured");
        }
        
        running_.store(true);
        
        // Start components in order
        receiver_.start(config_.network_thread_cpu);
        dispatcher_.start(config_.dispatcher_thread_cpu);
        
        // Start parsing thread
        parse_thread_ = std::thread(&CoreEngine::parse_loop, this);
        
        if (config_.parser_thread_cpu >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(config_.parser_thread_cpu, &cpuset);
            pthread_setaffinity_np(parse_thread_.native_handle(),
                                  sizeof(cpu_set_t), &cpuset);
        }
    }
    
    // Stop the core engine
    void stop() {
        if (!running_.load()) return;
        
        running_.store(false);
        
        // Stop parsing thread
        if (parse_thread_.joinable()) {
            parse_thread_.join();
        }
        
        // Stop components
        dispatcher_.stop();
        receiver_.stop();
        
        // Reset parser
        if (parser_) {
            parser_->reset();
        }
    }
    
    // Get combined statistics
    Statistics get_stats() const {
        Statistics combined = stats_;
        
        // Add receiver stats
        const auto& recv_stats = receiver_.get_stats();
        combined.packets_received = recv_stats.packets_received;
        combined.packets_dropped = recv_stats.packets_dropped;
        
        // Add dispatcher stats
        const auto& disp_stats = dispatcher_.get_stats();
        combined.messages_dispatched = disp_stats.messages_dispatched;
        
        return combined;
    }
    
    // Check if running
    bool is_running() const noexcept {
        return running_.load();
    }
    
    // Get number of subscribers
    size_t subscriber_count() const noexcept {
        return dispatcher_.subscriber_count();
    }
    
private:
    void parse_loop() {
        MessageView raw_packet;
        std::vector<NormalizedMessage> messages(config_.max_messages_per_packet);
        
        while (running_.load(std::memory_order_relaxed)) {
            // Read packet from network ring buffer
            if (receiver_.read_packet(raw_packet)) {
                const Timestamp parse_start = get_timestamp();
                
                // Parse packet into normalized messages
                size_t count = parser_->parse(raw_packet, 
                                             messages.data(), 
                                             messages.size());
                
                // Dispatch all parsed messages
                for (size_t i = 0; i < count; ++i) {
                    dispatcher_.dispatch(messages[i]);
                }
                
                // Update stats
                stats_.messages_parsed += count;
                if (count == 0) {
                    stats_.parse_errors++;
                }
                
                const Timestamp parse_end = get_timestamp();
                stats_.update_latency(parse_end - parse_start);
                
            } else {
                // No packets available, yield
                std::this_thread::yield();
            }
        }
    }
    
    CoreConfig config_;
    UDPReceiver receiver_;
    Dispatcher dispatcher_;
    std::unique_ptr<IParser> parser_;
    std::thread parse_thread_;
    std::atomic<bool> running_;
    Statistics stats_;
};

} // namespace hft::core