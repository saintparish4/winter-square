#pragma once

#include "common/types.hpp"
#include "structures/lock_free_queue.hpp"
#include "memory/object_pool.hpp"
#include <atomic>
#include <thread>
#include <functional>
#include <stdexcept>

#ifdef __linux__
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sched.h>
#include <sys/epoll.h>
#endif

namespace hft {

struct alignas(CACHE_LINE_SIZE) NetworkMessage {
    static constexpr size_t MAX_PAYLOAD_SIZE = 1500; // MTU
    
    // Message metadata
    Timestamp receive_timestamp;
    Timestamp kernel_timestamp;     // SO_TIMESTAMPING
    uint16_t payload_size;
    uint16_t source_port;
    uint32_t source_ip;
    uint32_t sequence_number;       // For gap detection
    ErrorCode error_code;
    uint8_t padding[3];
    
    // Payload data (aligned for efficient access)
    alignas(8) char payload[MAX_PAYLOAD_SIZE];
    
    NetworkMessage() noexcept : receive_timestamp{}, kernel_timestamp{}
                               , payload_size(0), source_port(0), source_ip(0)
                               , sequence_number(0), error_code(ErrorCode::SUCCESS) {}
    
    // Helper methods
    FORCE_INLINE bool is_valid() const noexcept {
        return payload_size > 0 && payload_size <= MAX_PAYLOAD_SIZE;
    }
    
    FORCE_INLINE Duration get_receive_latency() const noexcept {
        if (kernel_timestamp.time_since_epoch().count() > 0) {
            return receive_timestamp - kernel_timestamp;
        }
        return Duration::zero();
    }
};

class UDPReceiver {
private:
    int socket_fd_ = -1;
    struct sockaddr_in local_addr_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::thread receiver_thread_;
    Config config_;
    
    // Use SPSC queue for better performance (single receiver thread)
    SPSCLockFreeQueue<NetworkMessage*, 65536> message_queue_;
    
    // Object pool for zero-allocation receives (configurable size)
    std::unique_ptr<ObjectPool<NetworkMessage>> message_pool_;
    
    // Performance statistics (separate cache lines)
    CACHE_ALIGNED std::atomic<uint64_t> messages_received_{0};
    CACHE_ALIGNED std::atomic<uint64_t> bytes_received_{0};
    CACHE_ALIGNED std::atomic<uint64_t> drops_{0};
    CACHE_ALIGNED std::atomic<uint64_t> errors_{0};
    CACHE_ALIGNED std::atomic<uint64_t> sequence_gaps_{0};
    CACHE_ALIGNED std::atomic<uint32_t> last_sequence_{0};
    
    // Retry and recovery state
    CACHE_ALIGNED std::atomic<uint32_t> consecutive_errors_{0};
    CACHE_ALIGNED std::atomic<uint32_t> reconnect_attempts_{0};
    Timestamp last_error_time_{};
    
    // Adaptive sizing
    CACHE_ALIGNED std::atomic<uint64_t> peak_queue_usage_{0};
    CACHE_ALIGNED std::atomic<uint64_t> pool_expansions_{0};
    
    // Socket configuration helpers
    bool setup_socket() noexcept;
    bool configure_socket_options() noexcept;
    bool bind_socket() noexcept;
    bool set_cpu_affinity() noexcept;
    
    // Retry and recovery helpers
    bool retry_operation(std::function<bool()> operation, const char* operation_name) noexcept;
    void handle_error(const char* context) noexcept;
    bool should_attempt_recovery() const noexcept;
    void reset_error_state() noexcept;
    
    // Adaptive sizing helpers
    void check_and_expand_pool() noexcept;
    void update_usage_statistics() noexcept;
    
    // Main receive loop
    void receive_loop() noexcept;
    
    // Message processing
    FORCE_INLINE HOT_PATH bool process_received_data(const char* data, size_t len, 
                                                    const struct sockaddr_in& src_addr) noexcept {
        NetworkMessage* msg = message_pool_->construct();
        if (UNLIKELY(!msg)) {
            drops_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        
        // Fill message data
        msg->receive_timestamp = std::chrono::steady_clock::now();
        msg->payload_size = static_cast<uint16_t>(std::min(len, size_t(NetworkMessage::MAX_PAYLOAD_SIZE)));
        msg->source_ip = ntohl(src_addr.sin_addr.s_addr);
        msg->source_port = ntohs(src_addr.sin_port);
        
        // Copy payload
        std::memcpy(msg->payload, data, msg->payload_size);
        
        // Check for sequence gaps (assuming first 4 bytes are sequence number)
        if (len >= 4) {
            uint32_t seq = ntohl(*reinterpret_cast<const uint32_t*>(data));
            uint32_t expected = last_sequence_.load(std::memory_order_relaxed) + 1;
            
            if (seq != expected && expected != 1) { // Skip check for first message
                sequence_gaps_.fetch_add(seq - expected, std::memory_order_relaxed);
            }
            
            last_sequence_.store(seq, std::memory_order_relaxed);
            msg->sequence_number = seq;
        }
        
        // Try to enqueue message
        if (LIKELY(message_queue_.try_enqueue(msg))) {
            messages_received_.fetch_add(1, std::memory_order_relaxed);
            bytes_received_.fetch_add(len, std::memory_order_relaxed);
            return true;
        } else {
            // Queue full, drop message
            message_pool_->destroy(msg);
            drops_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

public:
    struct Config {
        const char* interface_ip = "0.0.0.0";
        uint16_t port = 12345;
        int cpu_affinity = -1;          // -1 means no affinity
        size_t socket_buffer_size = 64 * 1024 * 1024; // 64MB
        bool enable_timestamping = true;
        bool enable_busy_polling = true;
        bool enable_packet_mmap = false; // Use PACKET_MMAP for better performance
        uint32_t busy_poll_budget = 64; // Number of packets to process per poll
        int socket_priority = 7;        // SO_PRIORITY
        bool enable_reuse_port = false; // SO_REUSEPORT for load balancing
        Duration receive_timeout = std::chrono::milliseconds(1);
        
        // Pool and queue sizing
        size_t initial_pool_size = 100000;
        size_t max_pool_size = 1000000;
        size_t queue_size = 65536;
        bool enable_adaptive_sizing = true;
        
        // Retry and recovery configuration
        uint32_t max_retry_attempts = 3;
        Duration retry_backoff_base = std::chrono::milliseconds(10);
        Duration max_retry_backoff = std::chrono::seconds(5);
        uint32_t max_consecutive_errors = 100;
        bool enable_auto_recovery = true;
        Duration recovery_check_interval = std::chrono::seconds(1);
        
        // Graceful degradation settings
        bool require_timestamping = false;  // Fail if timestamping unavailable
        bool require_cpu_affinity = false;  // Fail if CPU affinity unavailable
        bool require_high_priority = false; // Fail if socket priority unavailable
        bool fallback_on_optimization_failure = true;
        
        // Validation function
        bool is_valid() const noexcept;
    };
    
    explicit UDPReceiver(const Config& config = {}) : config_(config) {
        // Validate configuration first
        if (!config_.is_valid()) {
            throw std::invalid_argument("Invalid UDPReceiver configuration");
        }
        
        // Create configurable-sized pools
        message_pool_ = std::make_unique<ObjectPool<NetworkMessage>>(config_.initial_pool_size);
        
        // Clear address structure
        std::memset(&local_addr_, 0, sizeof(local_addr_));
        local_addr_.sin_family = AF_INET;
        local_addr_.sin_port = htons(config_.port);
        
        if (inet_pton(AF_INET, config_.interface_ip, &local_addr_.sin_addr) <= 0) {
            local_addr_.sin_addr.s_addr = INADDR_ANY;
        }
    }
    
    ~UDPReceiver() {
        stop();
        cleanup();
    }
    
    // Non-copyable, non-movable
    UDPReceiver(const UDPReceiver&) = delete;
    UDPReceiver& operator=(const UDPReceiver&) = delete;
    UDPReceiver(UDPReceiver&&) = delete;
    UDPReceiver& operator=(UDPReceiver&&) = delete;
    
    bool initialize() noexcept;
    void cleanup() noexcept;
    
    bool start() noexcept {
        if (running_.load(std::memory_order_acquire)) {
            return true; // Already running
        }
        
        if (!initialized_.load(std::memory_order_acquire)) {
            if (!initialize()) {
                return false;
            }
        }
        
        running_.store(true, std::memory_order_release);
        
        try {
            receiver_thread_ = std::thread(&UDPReceiver::receive_loop, this);
            return true;
        } catch (...) {
            running_.store(false, std::memory_order_release);
            return false;
        }
    }
    
    void stop() noexcept {
        if (!running_.load(std::memory_order_acquire)) {
            return; // Already stopped
        }
        
        running_.store(false, std::memory_order_release);
        
        // Wake up receiver thread if blocked in recv
        if (socket_fd_ >= 0) {
            shutdown(socket_fd_, SHUT_RD);
        }
        
        if (receiver_thread_.joinable()) {
            receiver_thread_.join();
        }
    }
    
    // Message retrieval (non-blocking)
    FORCE_INLINE HOT_PATH NetworkMessage* try_get_message() noexcept {
        NetworkMessage* msg = nullptr;
        if (message_queue_.try_dequeue(msg)) {
            return msg;
        }
        return nullptr;
    }
    
    // Batch message retrieval for better performance
    FORCE_INLINE size_t try_get_messages(NetworkMessage** messages, size_t max_count) noexcept {
        size_t count = 0;
        for (size_t i = 0; i < max_count; ++i) {
            if (!message_queue_.try_dequeue(messages[i])) {
                break;
            }
            ++count;
        }
        return count;
    }
    
    // Return message to pool
    FORCE_INLINE HOT_PATH void return_message(NetworkMessage* msg) noexcept {
        if (LIKELY(msg)) {
            message_pool_->destroy(msg);
        }
    }
    
    // Batch return for efficiency
    FORCE_INLINE void return_messages(NetworkMessage** messages, size_t count) noexcept {
        for (size_t i = 0; i < count; ++i) {
            return_message(messages[i]);
            messages[i] = nullptr;
        }
    }
    
    // Statistics and monitoring
    struct Stats {
        uint64_t messages_received;
        uint64_t bytes_received;
        uint64_t drops;
        uint64_t errors;
        uint64_t sequence_gaps;
        size_t queue_size;
        size_t pool_available;
        size_t pool_allocated;
        bool is_running;
        
        // Enhanced statistics for monitoring
        uint32_t consecutive_errors;
        uint32_t reconnect_attempts;
        uint64_t peak_queue_usage;
        uint64_t pool_expansions;
        bool recovery_in_progress;
        Duration time_since_last_error;
    };
    
    Stats get_stats() const noexcept {
        auto now = std::chrono::steady_clock::now();
        return {
            messages_received_.load(std::memory_order_relaxed),
            bytes_received_.load(std::memory_order_relaxed),
            drops_.load(std::memory_order_relaxed),
            errors_.load(std::memory_order_relaxed),
            sequence_gaps_.load(std::memory_order_relaxed),
            message_queue_.size(),
            message_pool_->available_count(),
            message_pool_->allocated_count(),
            running_.load(std::memory_order_relaxed),
            
            // Enhanced statistics
            consecutive_errors_.load(std::memory_order_relaxed),
            reconnect_attempts_.load(std::memory_order_relaxed),
            peak_queue_usage_.load(std::memory_order_relaxed),
            pool_expansions_.load(std::memory_order_relaxed),
            consecutive_errors_.load(std::memory_order_relaxed) > 0,
            now - last_error_time_
        };
    }
    
    void reset_stats() noexcept {
        messages_received_.store(0, std::memory_order_relaxed);
        bytes_received_.store(0, std::memory_order_relaxed);
        drops_.store(0, std::memory_order_relaxed);
        errors_.store(0, std::memory_order_relaxed);
        sequence_gaps_.store(0, std::memory_order_relaxed);
        last_sequence_.store(0, std::memory_order_relaxed);
        
        // Reset enhanced statistics
        consecutive_errors_.store(0, std::memory_order_relaxed);
        reconnect_attempts_.store(0, std::memory_order_relaxed);
        peak_queue_usage_.store(0, std::memory_order_relaxed);
        pool_expansions_.store(0, std::memory_order_relaxed);
        last_error_time_ = {};
    }
    
    // Status checks
    FORCE_INLINE bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }
    
    FORCE_INLINE bool is_initialized() const noexcept {
        return initialized_.load(std::memory_order_acquire);
    }
    
    FORCE_INLINE bool queue_full() const noexcept {
        return message_queue_.full();
    }
    
    FORCE_INLINE bool pool_exhausted() const noexcept {
        return message_pool_->available_count() == 0;
    }
    
    // Configuration access
    FORCE_INLINE uint16_t get_port() const noexcept {
        return config_.port;
    }
    
    FORCE_INLINE const char* get_interface() const noexcept {
        return config_.interface_ip;
    }
    
    // Advanced features
    bool enable_multicast(const char* multicast_group) noexcept;
    bool set_dscp_marking(uint8_t dscp) noexcept;
    Duration get_average_latency() const noexcept;
    
    // Health check with enhanced criteria
    bool is_healthy() const noexcept {
        auto stats = get_stats();
        return is_running() && 
               !pool_exhausted() && 
               !queue_full() &&
               stats.consecutive_errors < config_.max_consecutive_errors &&
               !stats.recovery_in_progress;
    }
    
    // Force recovery attempt
    bool force_recovery() noexcept;
    
    // Get current pool utilization (0.0 to 1.0)
    FORCE_INLINE double get_pool_utilization() const noexcept {
        size_t allocated = message_pool_->allocated_count();
        size_t total = allocated + message_pool_->available_count();
        return total > 0 ? static_cast<double>(allocated) / total : 0.0;
    }
    
    // Get current queue utilization (0.0 to 1.0) 
    FORCE_INLINE double get_queue_utilization() const noexcept {
        return static_cast<double>(message_queue_.size()) / config_.queue_size;
    }
};

// Multi-port UDP receiver for handling multiple data feeds
class MultiPortUDPReceiver {
private:
    std::vector<std::unique_ptr<UDPReceiver>> receivers_;
    std::atomic<bool> running_{false};
    
public:
    struct PortConfig {
        uint16_t port;
        const char* interface_ip = "0.0.0.0";
        int cpu_affinity = -1;
    };
    
    explicit MultiPortUDPReceiver(const std::vector<PortConfig>& port_configs) {
        UDPReceiver::Config base_config;
        
        for (const auto& port_config : port_configs) {
            base_config.port = port_config.port;
            base_config.interface_ip = port_config.interface_ip;
            base_config.cpu_affinity = port_config.cpu_affinity;
            
            receivers_.emplace_back(std::make_unique<UDPReceiver>(base_config));
        }
    }
    
    bool start_all() noexcept {
        bool success = true;
        for (auto& receiver : receivers_) {
            if (!receiver->start()) {
                success = false;
            }
        }
        running_.store(success, std::memory_order_release);
        return success;
    }
    
    void stop_all() noexcept {
        running_.store(false, std::memory_order_release);
        for (auto& receiver : receivers_) {
            receiver->stop();
        }
    }
    
    // Round-robin message retrieval from all receivers
    FORCE_INLINE NetworkMessage* try_get_any_message() noexcept {
        for (auto& receiver : receivers_) {
            NetworkMessage* msg = receiver->try_get_message();
            if (msg) return msg;
        }
        return nullptr;
    }
    
    // Get combined statistics
    UDPReceiver::Stats get_combined_stats() const noexcept {
        UDPReceiver::Stats combined = {};
        
        for (const auto& receiver : receivers_) {
            auto stats = receiver->get_stats();
            combined.messages_received += stats.messages_received;
            combined.bytes_received += stats.bytes_received;
            combined.drops += stats.drops;
            combined.errors += stats.errors;
            combined.sequence_gaps += stats.sequence_gaps;
            combined.queue_size += stats.queue_size;
            combined.pool_available += stats.pool_available;
            combined.pool_allocated += stats.pool_allocated;
        }
        
        combined.is_running = running_.load(std::memory_order_relaxed);
        return combined;
    }
    
    size_t get_receiver_count() const noexcept { return receivers_.size(); }
    
    UDPReceiver* get_receiver(size_t index) noexcept {
        return index < receivers_.size() ? receivers_[index].get() : nullptr;
    }
};

} // namespace hft