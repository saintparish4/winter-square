#pragma once

#include "subscriber_interface.hpp"
#include "lockfree_queue.hpp"
#include "../types.hpp"
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>

namespace hft::core {

// Dispatcher distributes normalized messages to multiple subscribers
// Uses lock-free queues for each subscriber to minimize latency
class Dispatcher {
public:
    Dispatcher() : running_(false), stats_() {}
    
    ~Dispatcher() {
        stop();
    }
    
    // Add a subscriber (not thread-safe, call before start())
    void add_subscriber(std::unique_ptr<ISubscriber> subscriber) {
        subscribers_.push_back(std::move(subscriber));
        queues_.push_back(std::make_unique<SPSCQueue<NormalizedMessage>>());
    }
    
    // Start dispatcher thread
    void start(int cpu_affinity = -1) {
        if (running_.load()) return;
        
        // Initialize all subscribers
        for (auto& sub : subscribers_) {
            sub->initialize();
        }
        
        running_.store(true);
        dispatch_thread_ = std::thread(&Dispatcher::dispatch_loop, this);
        
        // Set CPU affinity if requested
        if (cpu_affinity >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cpu_affinity, &cpuset);
            pthread_setaffinity_np(dispatch_thread_.native_handle(), 
                                  sizeof(cpu_set_t), &cpuset);
        }
    }
    
    // Stop dispatcher thread
    void stop() {
        if (!running_.load()) return;
        
        running_.store(false);
        if (dispatch_thread_.joinable()) {
            dispatch_thread_.join();
        }
        
        // Shutdown all subscribers
        for (auto& sub : subscribers_) {
            sub->shutdown();
        }
    }
    
    // Dispatch a message to all subscribers
    // Called by parser thread
    void dispatch(const NormalizedMessage& msg) noexcept {
        const Timestamp now = get_timestamp();
        const uint64_t latency = now - msg.local_timestamp;
        
        // Push to all subscriber queues
        for (auto& queue : queues_) {
            if (!queue->push(msg)) {
                stats_.packets_dropped++;
            }
        }
        
        stats_.messages_dispatched++;
        stats_.update_latency(latency);
    }
    
    // Get statistics
    const Statistics& get_stats() const noexcept {
        return stats_;
    }
    
    // Get number of subscribers
    size_t subscriber_count() const noexcept {
        return subscribers_.size();
    }
    
private:
    void dispatch_loop() {
        NormalizedMessage msg;
        
        while (running_.load(std::memory_order_relaxed)) {
            bool any_activity = false;
            
            // Process messages for each subscriber
            for (size_t i = 0; i < subscribers_.size(); ++i) {
                auto& queue = queues_[i];
                auto& subscriber = subscribers_[i];
                
                while (queue->pop(msg)) {
                    any_activity = true;
                    
                    // Deliver to subscriber
                    if (!subscriber->on_message(msg)) {
                        // Subscriber returned false - wants to unsubscribe
                        // TODO: Handle unsubscription
                    }
                }
            }
            
            // Yield if no activity to reduce CPU usage
            if (!any_activity) {
                std::this_thread::yield();
            }
        }
    }
    
    std::vector<std::unique_ptr<ISubscriber>> subscribers_;
    std::vector<std::unique_ptr<SPSCQueue<NormalizedMessage>>> queues_;
    std::thread dispatch_thread_;
    std::atomic<bool> running_;
    Statistics stats_;
};

} // namespace hft::core