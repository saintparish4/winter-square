#pragma once

#include "common/types.hpp"
#include "structures/lock_free_queue.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <atomic>
#include <thread>

namespace hft {

struct NetworkMessage {
    static constexpr size_t MAX_PAYLOAD_SIZE = 1500; // MTU
    
    Timestamp receive_timestamp;
    uint16_t payload_size;
    alignas(8) char payload[MAX_PAYLOAD_SIZE];
    
    NetworkMessage() : receive_timestamp{}, payload_size(0) {}
};

class UDPReceiver {
private:
    int socket_fd_;
    struct sockaddr_in local_addr_;
    std::atomic<bool> running_{false};
    std::thread receiver_thread_;
    
    // Lock-free queue for received messages
    LockFreeQueue<NetworkMessage, 65536> message_queue_;
    
    // Message pool for zero-allocation receives
    static constexpr size_t MESSAGE_POOL_SIZE = 100000;
    NetworkMessage message_pool_[MESSAGE_POOL_SIZE];
    std::atomic<size_t> pool_index_{0};
    
    // Performance statistics
    CACHE_ALIGNED std::atomic<uint64_t> messages_received_{0};
    CACHE_ALIGNED std::atomic<uint64_t> bytes_received_{0};
    CACHE_ALIGNED std::atomic<uint64_t> drops_{0};
    
    void receive_loop() noexcept;
    FORCE_INLINE NetworkMessage* get_message_from_pool() noexcept;

public:
    struct Config {
        const char* interface_ip = "0.0.0.0";
        uint16_t port = 12345;
        int cpu_affinity = -1;  // -1 means no affinity
        size_t socket_buffer_size = 64 * 1024 * 1024; // 64MB
        bool enable_kernel_bypass = true;
        bool enable_busy_polling = true;
    };
    
    explicit UDPReceiver(const Config& config);
    ~UDPReceiver();
    
    // Non-copyable, non-movable
    UDPReceiver(const UDPReceiver&) = delete;
    UDPReceiver& operator=(const UDPReceiver&) = delete;
    UDPReceiver(UDPReceiver&&) = delete;
    UDPReceiver& operator=(UDPReceiver&&) = delete;
    
    bool start() noexcept;
    void stop() noexcept;
    
    // Get next received message (non-blocking)
    FORCE_INLINE NetworkMessage* try_get_message() noexcept {
        return message_queue_.try_dequeue();
    }
    
    // Return message to pool
    FORCE_INLINE void return_message(NetworkMessage* msg) noexcept {
        // In a real implementation, we'd return to pool
        // For simplicity, we'll just ignore here
        (void)msg;
    }
    
    // Statistics
    FORCE_INLINE uint64_t get_messages_received() const noexcept {
        return messages_received_.load(std::memory_order_relaxed);
    }
    
    FORCE_INLINE uint64_t get_bytes_received() const noexcept {
        return bytes_received_.load(std::memory_order_relaxed);
    }
    
    FORCE_INLINE uint64_t get_drops() const noexcept {
        return drops_.load(std::memory_order_relaxed);
    }
    
    FORCE_INLINE bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }
};

} // namespace hft
