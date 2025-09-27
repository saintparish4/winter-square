#include "network/udp_receiver.hpp"
#include "utils/cpu_affinity.hpp"
#include "utils/high_precision_timer.hpp"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <cstring>
#include <algorithm>
#include <thread>
#include <cmath>
#include <cstdlib>

#ifdef __linux__
#include <linux/net_tstamp.h>
#endif

namespace hft {

// Configuration validation implementation
bool UDPReceiver::Config::is_valid() const noexcept {
    // Port validation
    if (port == 0 || port > 65535) return false;
    
    // IP address validation (basic check)
    if (!interface_ip || strlen(interface_ip) == 0) return false;
    
    // Buffer size validation
    if (socket_buffer_size < 1024 || socket_buffer_size > 1024*1024*1024) return false;
    
    // Pool size validation
    if (initial_pool_size == 0 || initial_pool_size > max_pool_size) return false;
    if (max_pool_size > 10000000) return false; // 10M max
    
    // Queue size validation (must be power of 2 for lock-free queue)
    if (queue_size == 0 || (queue_size & (queue_size - 1)) != 0) return false;
    
    // Retry configuration validation
    if (max_retry_attempts > 100) return false;
    if (max_consecutive_errors == 0) return false;
    
    // Timeout validation
    if (receive_timeout.count() < 0) return false;
    if (retry_backoff_base.count() < 0 || max_retry_backoff.count() < 0) return false;
    if (recovery_check_interval.count() <= 0) return false;
    
    // Priority validation
    if (socket_priority < 0 || socket_priority > 7) return false;
    
    // CPU affinity validation
    if (cpu_affinity < -1 || cpu_affinity > 1023) return false; // Reasonable CPU limit
    
    return true;
}

// Retry operation with exponential backoff
bool UDPReceiver::retry_operation(std::function<bool()> operation, const char* operation_name) noexcept {
    for (uint32_t attempt = 0; attempt < config_.max_retry_attempts; ++attempt) {
        if (operation()) {
            if (attempt > 0) {
                // Reset error state on successful retry
                reset_error_state();
            }
            return true;
        }
        
        if (attempt < config_.max_retry_attempts - 1) {
            // Calculate exponential backoff with jitter
            auto backoff = config_.retry_backoff_base * (1 << attempt);
            backoff = std::min(backoff, config_.max_retry_backoff);
            
            // Add jitter (±25%)
            auto jitter = backoff / 4;
            auto jittered_backoff = backoff + Duration(rand() % (jitter.count() * 2) - jitter.count());
            
            std::this_thread::sleep_for(jittered_backoff);
        }
    }
    
    handle_error(operation_name);
    return false;
}

// Handle error with tracking
void UDPReceiver::handle_error(const char* context) noexcept {
    errors_.fetch_add(1, std::memory_order_relaxed);
    consecutive_errors_.fetch_add(1, std::memory_order_relaxed);
    last_error_time_ = std::chrono::steady_clock::now();
    
    // Log error (would typically use a logging system)
    // For now, we just track the statistics
}

// Check if recovery should be attempted
bool UDPReceiver::should_attempt_recovery() const noexcept {
    if (!config_.enable_auto_recovery) return false;
    
    auto consecutive = consecutive_errors_.load(std::memory_order_relaxed);
    if (consecutive < config_.max_consecutive_errors) return false;
    
    auto now = std::chrono::steady_clock::now();
    auto time_since_error = now - last_error_time_;
    
    return time_since_error >= config_.recovery_check_interval;
}

// Reset error state
void UDPReceiver::reset_error_state() noexcept {
    consecutive_errors_.store(0, std::memory_order_relaxed);
    last_error_time_ = {};
}

// Adaptive pool sizing
void UDPReceiver::check_and_expand_pool() noexcept {
    if (!config_.enable_adaptive_sizing) return;
    
    double utilization = get_pool_utilization();
    size_t current_size = message_pool_->allocated_count() + message_pool_->available_count();
    
    // Expand if utilization is high and we haven't reached max size
    if (utilization > 0.8 && current_size < config_.max_pool_size) {
        size_t new_size = std::min(current_size * 2, config_.max_pool_size);
        if (message_pool_->expand(new_size - current_size)) {
            pool_expansions_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// Update usage statistics
void UDPReceiver::update_usage_statistics() noexcept {
    size_t queue_size = message_queue_.size();
    uint64_t current_peak = peak_queue_usage_.load(std::memory_order_relaxed);
    
    if (queue_size > current_peak) {
        peak_queue_usage_.store(queue_size, std::memory_order_relaxed);
    }
}

// Setup socket with retry logic
bool UDPReceiver::setup_socket() noexcept {
    return retry_operation([this]() -> bool {
        socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        return socket_fd_ >= 0;
    }, "socket_creation");
}

// Configure socket options with graceful degradation
bool UDPReceiver::configure_socket_options() noexcept {
    bool all_succeeded = true;
    
    // Essential options (must succeed)
    int reuse = 1;
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        if (!config_.fallback_on_optimization_failure) {
            handle_error("SO_REUSEADDR");
            return false;
        }
        all_succeeded = false;
    }
    
    // Buffer size (essential for performance)
    int buffer_size = static_cast<int>(config_.socket_buffer_size);
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) < 0) {
        if (!config_.fallback_on_optimization_failure) {
            handle_error("SO_RCVBUF");
            return false;
        }
        all_succeeded = false;
    }
    
    // Optional optimizations (graceful degradation)
    if (config_.enable_timestamping) {
#ifdef __linux__
        int timestamp_flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
        if (setsockopt(socket_fd_, SOL_SOCKET, SO_TIMESTAMPING, &timestamp_flags, sizeof(timestamp_flags)) < 0) {
            if (config_.require_timestamping) {
                handle_error("SO_TIMESTAMPING");
                return false;
            }
            // Continue without timestamping
        }
#else
        if (config_.require_timestamping) {
            handle_error("SO_TIMESTAMPING_NOT_SUPPORTED");
            return false;
        }
#endif
    }
    
    // Socket priority
    int priority = config_.socket_priority;
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority)) < 0) {
        if (config_.require_high_priority) {
            handle_error("SO_PRIORITY");
            return false;
        }
        all_succeeded = false;
    }
    
    // Reuse port for load balancing
    if (config_.enable_reuse_port) {
        int reuse_port = 1;
        if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEPORT, &reuse_port, sizeof(reuse_port)) < 0) {
            // This is optional, continue without it
            all_succeeded = false;
        }
    }
    
    return true; // Return true even if some optional features failed
}

// Bind socket with retry
bool UDPReceiver::bind_socket() noexcept {
    return retry_operation([this]() -> bool {
        return bind(socket_fd_, reinterpret_cast<struct sockaddr*>(&local_addr_), 
                   sizeof(local_addr_)) >= 0;
    }, "socket_bind");
}

// Set CPU affinity with graceful degradation
bool UDPReceiver::set_cpu_affinity() noexcept {
    if (config_.cpu_affinity < 0) return true; // No affinity requested
    
    // This would typically use the cpu_affinity utility
    // For now, we'll simulate the call
    bool success = true; // configure_hft_thread(config_.cpu_affinity);
    
    if (!success && config_.require_cpu_affinity) {
        handle_error("cpu_affinity");
        return false;
    }
    
    return true;
}

bool UDPReceiver::initialize() noexcept {
    if (initialized_.load(std::memory_order_acquire)) {
        return true;
    }
    
    // Setup socket with retries
    if (!setup_socket()) {
        return false;
    }
    
    // Configure socket options with graceful degradation
    if (!configure_socket_options()) {
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }
    
    // Set non-blocking mode (essential)
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    if (fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        handle_error("non_blocking_mode");
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }
    
    // Bind socket
    if (!bind_socket()) {
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }
    
    initialized_.store(true, std::memory_order_release);
    return true;
}

void UDPReceiver::cleanup() noexcept {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    initialized_.store(false, std::memory_order_release);
}

void UDPReceiver::stop() noexcept {
    running_.store(false, std::memory_order_release);
    
    if (receiver_thread_.joinable()) {
        receiver_thread_.join();
    }
}

void UDPReceiver::receive_loop() noexcept {
    // Set CPU affinity with graceful degradation
    set_cpu_affinity();
    
    char buffer[2048];
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);
    uint32_t consecutive_errors = 0;
    
    while (running_.load(std::memory_order_acquire)) {
        // Check if recovery is needed
        if (should_attempt_recovery()) {
            if (force_recovery()) {
                consecutive_errors = 0;
            }
        }
        
        ssize_t bytes_received = recvfrom(socket_fd_, buffer, sizeof(buffer), 0,
                                         reinterpret_cast<struct sockaddr*>(&sender_addr),
                                         &sender_len);
        
        if (bytes_received > 0) {
            // Get timestamp as early as possible
            Timestamp receive_time = std::chrono::steady_clock::now();
            
            // Get message from pool
            NetworkMessage* msg = message_pool_->construct();
            if (msg) {
                msg->receive_timestamp = receive_time;
                msg->payload_size = static_cast<uint16_t>(bytes_received);
                msg->source_ip = ntohl(sender_addr.sin_addr.s_addr);
                msg->source_port = ntohs(sender_addr.sin_port);
                std::memcpy(msg->payload, buffer, bytes_received);
                
                // Try to enqueue message
                if (message_queue_.try_enqueue(msg)) {
                    messages_received_.fetch_add(1, std::memory_order_relaxed);
                    bytes_received_.fetch_add(bytes_received, std::memory_order_relaxed);
                    consecutive_errors = 0; // Reset on successful processing
                } else {
                    drops_.fetch_add(1, std::memory_order_relaxed);
                    message_pool_->destroy(msg);
                }
            } else {
                drops_.fetch_add(1, std::memory_order_relaxed);
                // Check if we need to expand the pool
                check_and_expand_pool();
            }
            
            // Update usage statistics periodically
            if ((messages_received_.load(std::memory_order_relaxed) & 0xFFF) == 0) {
                update_usage_statistics();
            }
            
        } else if (bytes_received < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                // Real error occurred
                consecutive_errors++;
                handle_error("recvfrom");
                
                if (consecutive_errors > config_.max_consecutive_errors) {
                    break; // Exit receive loop
                }
            }
        }
        
        // Brief pause to prevent 100% CPU usage
        CPU_PAUSE();
    }
}

// Force recovery attempt
bool UDPReceiver::force_recovery() noexcept {
    reconnect_attempts_.fetch_add(1, std::memory_order_relaxed);
    
    // Close current socket
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    
    // Attempt to reinitialize
    initialized_.store(false, std::memory_order_release);
    
    if (initialize()) {
        reset_error_state();
        return true;
    }
    
    return false;
}

} // namespace hft
