#pragma once

#include "../types.hpp"
#include "../distribution/lockfree_queue.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <atomic>
#include <memory>

namespace hft::core {

// UDP receiver configuration
struct UDPConfig {
    std::string interface_ip{"0.0.0.0"};
    std::string multicast_group{"239.1.1.1"};
    uint16_t port{10000};
    size_t buffer_size{config::MAX_PACKET_SIZE * 1024};
    bool enable_timestamps{true};
    
    UDPConfig() = default;
};

// Ring buffer for packet storage
class PacketRingBuffer {
public:
    explicit PacketRingBuffer(size_t size = config::PACKET_RING_SIZE) 
        : size_(size), head_(0), tail_(0) {
        // Allocate aligned memory for packets
        buffer_ = static_cast<uint8_t*>(
            aligned_alloc(config::PAGE_SIZE, size * config::MAX_PACKET_SIZE)
        );
        if (!buffer_) {
            throw std::bad_alloc();
        }
    }
    
    ~PacketRingBuffer() {
        if (buffer_) {
            free(buffer_);
        }
    }
    
    // Get pointer to write next packet
    uint8_t* get_write_ptr() noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        return buffer_ + (head * config::MAX_PACKET_SIZE);
    }
    
    // Commit written packet
    bool commit_write(uint32_t length, Timestamp ts) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (head + 1) % size_;
        
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;  // Buffer full
        }
        
        lengths_[head] = length;
        timestamps_[head] = ts;
        head_.store(next_head, std::memory_order_release);
        return true;
    }
    
    // Read next packet
    bool read(MessageView& view) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;  // Buffer empty
        }
        
        view.data = buffer_ + (tail * config::MAX_PACKET_SIZE);
        view.length = lengths_[tail];
        view.timestamp = timestamps_[tail];
        view.sequence = static_cast<uint32_t>(tail);
        
        tail_.store((tail + 1) % size_, std::memory_order_release);
        return true;
    }
    
    bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire) == 
               head_.load(std::memory_order_acquire);
    }
    
private:
    size_t size_;
    uint8_t* buffer_;
    std::unique_ptr<uint32_t[]> lengths_{std::make_unique<uint32_t[]>(size_)};
    std::unique_ptr<Timestamp[]> timestamps_{std::make_unique<Timestamp[]>(size_)};
    
    alignas(config::CACHELINE_SIZE) std::atomic<size_t> head_;
    alignas(config::CACHELINE_SIZE) std::atomic<size_t> tail_;
};

// UDP receiver - captures market data packets
class UDPReceiver {
public:
    explicit UDPReceiver(const UDPConfig& config = UDPConfig{})
        : config_(config), running_(false), socket_fd_(-1), 
          ring_buffer_(), stats_() {}
    
    ~UDPReceiver() {
        stop();
    }
    
    // Initialize socket and join multicast group
    bool initialize() {
        // Create UDP socket
        socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd_ < 0) {
            return false;
        }
        
        // Set socket options for performance
        int reuse = 1;
        setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        
        // Increase receive buffer
        int bufsize = config_.buffer_size;
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
        
        // Enable timestamps if requested
        if (config_.enable_timestamps) {
            int enable = 1;
            setsockopt(socket_fd_, SOL_SOCKET, SO_TIMESTAMP, &enable, sizeof(enable));
        }
        
        // Bind to port
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config_.port);
        addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        
        // Join multicast group
        struct ip_mreq mreq{};
        inet_pton(AF_INET, config_.multicast_group.c_str(), &mreq.imr_multiaddr);
        inet_pton(AF_INET, config_.interface_ip.c_str(), &mreq.imr_interface);
        
        if (setsockopt(socket_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, 
                      &mreq, sizeof(mreq)) < 0) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        
        return true;
    }
    
    // Start receiving thread
    void start(int cpu_affinity = -1) {
        if (running_.load() || socket_fd_ < 0) return;
        
        running_.store(true);
        receive_thread_ = std::thread(&UDPReceiver::receive_loop, this);
        
        // Set CPU affinity
        if (cpu_affinity >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cpu_affinity, &cpuset);
            pthread_setaffinity_np(receive_thread_.native_handle(),
                                  sizeof(cpu_set_t), &cpuset);
        }
    }
    
    // Stop receiving
    void stop() {
        if (!running_.load()) return;
        
        running_.store(false);
        if (receive_thread_.joinable()) {
            receive_thread_.join();
        }
        
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
    }
    
    // Read next packet from ring buffer
    bool read_packet(MessageView& view) noexcept {
        return ring_buffer_.read(view);
    }
    
    // Check if packets are available
    bool has_packets() const noexcept {
        return !ring_buffer_.empty();
    }
    
    // Get statistics
    const Statistics& get_stats() const noexcept {
        return stats_;
    }
    
private:
    void receive_loop() {
        while (running_.load(std::memory_order_relaxed)) {
            uint8_t* buffer = ring_buffer_.get_write_ptr();
            
            ssize_t received = recvfrom(socket_fd_, buffer, 
                                       config::MAX_PACKET_SIZE, 0,
                                       nullptr, nullptr);
            
            if (received > 0) {
                const Timestamp ts = get_timestamp();
                
                if (ring_buffer_.commit_write(received, ts)) {
                    stats_.packets_received++;
                } else {
                    stats_.packets_dropped++;
                }
            }
        }
    }
    
    UDPConfig config_;
    std::atomic<bool> running_;
    int socket_fd_;
    PacketRingBuffer ring_buffer_;
    std::thread receive_thread_;
    Statistics stats_;
};

} // namespace hft::core