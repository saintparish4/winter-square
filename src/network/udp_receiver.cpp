#include "network/udp_receiver.hpp"
#include "utils/cpu_affinity.hpp"
#include "utils/high_precision_timer.hpp"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>

namespace hft {

UDPReceiver::UDPReceiver(const Config& config) 
    : socket_fd_(-1) {
    
    // Initialize message pool
    for (size_t i = 0; i < MESSAGE_POOL_SIZE; ++i) {
        message_pool_[i] = NetworkMessage{};
    }
}

UDPReceiver::~UDPReceiver() {
    stop();
    if (socket_fd_ >= 0) {
        close(socket_fd_);
    }
}

bool UDPReceiver::start() noexcept {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    
    // Create socket
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        return false;
    }
    
    // Set socket options for high performance
    int reuse = 1;
    setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // Set large receive buffer
    int buffer_size = 64 * 1024 * 1024; // 64MB
    setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
    
    // Set non-blocking mode
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);
    
    // Bind to address
    memset(&local_addr_, 0, sizeof(local_addr_));
    local_addr_.sin_family = AF_INET;
    local_addr_.sin_addr.s_addr = INADDR_ANY;
    local_addr_.sin_port = htons(12345); // Default port
    
    if (bind(socket_fd_, reinterpret_cast<struct sockaddr*>(&local_addr_), 
             sizeof(local_addr_)) < 0) {
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }
    
    // Start receiver thread
    running_.store(true, std::memory_order_release);
    receiver_thread_ = std::thread(&UDPReceiver::receive_loop, this);
    
    return true;
}

void UDPReceiver::stop() noexcept {
    running_.store(false, std::memory_order_release);
    
    if (receiver_thread_.joinable()) {
        receiver_thread_.join();
    }
}

void UDPReceiver::receive_loop() noexcept {
    // Configure thread for high performance
    configure_hft_thread(2); // Use CPU 2 for network
    
    char buffer[2048];
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);
    
    while (running_.load(std::memory_order_acquire)) {
        ssize_t bytes_received = recvfrom(socket_fd_, buffer, sizeof(buffer), 0,
                                         reinterpret_cast<struct sockaddr*>(&sender_addr),
                                         &sender_len);
        
        if (bytes_received > 0) {
            // Get timestamp as early as possible
            Timestamp receive_time = HighPrecisionTimer::get_timestamp();
            
            // Get message from pool
            NetworkMessage* msg = get_message_from_pool();
            if (msg) {
                msg->receive_timestamp = receive_time;
                msg->payload_size = static_cast<uint16_t>(bytes_received);
                std::memcpy(msg->payload, buffer, bytes_received);
                
                // Try to enqueue message
                if (!message_queue_.try_enqueue(msg)) {
                    drops_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    messages_received_.fetch_add(1, std::memory_order_relaxed);
                    bytes_received_.fetch_add(bytes_received, std::memory_order_relaxed);
                }
            } else {
                drops_.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (bytes_received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            // Real error occurred
            break;
        }
        
        // Brief pause to prevent 100% CPU usage
        CPU_PAUSE();
    }
}

NetworkMessage* UDPReceiver::get_message_from_pool() noexcept {
    size_t index = pool_index_.fetch_add(1, std::memory_order_relaxed) % MESSAGE_POOL_SIZE;
    return &message_pool_[index];
}

} // namespace hft
