#pragma once

#include "common/types.hpp"
#include "memory/memory_pool.hpp"
#include <cstdint>
#include <atomic>
#include <array>
#include <stdexcept>

#ifdef __linux__
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#endif

namespace hft {

// DPDK-style packet processing interface
class KernelBypass {
public:
    // Optimized packet buffer with proper alignment
    struct alignas(CACHE_LINE_SIZE) PacketBuffer {
        static constexpr size_t MAX_PACKET_SIZE = 2048;
        static constexpr size_t HEADROOM = 128; // Space for headers
        
        // Metadata (fits in one cache line)
        uint16_t length;
        uint16_t data_offset;
        uint32_t hash;                    // RSS hash from hardware
        Timestamp timestamp;
        uint16_t port_id;
        uint16_t queue_id;
        uint32_t packet_type;            // Protocol flags
        uint64_t ol_flags;               // Offload flags
        
        // Packet data with headroom
        alignas(16) char data[HEADROOM + MAX_PACKET_SIZE];
        
        PacketBuffer() noexcept : length(0), data_offset(HEADROOM), hash(0)
                                , timestamp{}, port_id(0), queue_id(0)
                                , packet_type(0), ol_flags(0) {}
        
        FORCE_INLINE HOT_PATH char* get_data() noexcept {
            return data + data_offset;
        }
        
        FORCE_INLINE HOT_PATH const char* get_data() const noexcept {
            return data + data_offset;
        }
        
        FORCE_INLINE char* get_headroom() noexcept {
            return data;
        }
        
        FORCE_INLINE size_t available_headroom() const noexcept {
            return data_offset;
        }
        
        FORCE_INLINE size_t available_tailroom() const noexcept {
            return MAX_PACKET_SIZE - length;
        }
        
        // Adjust data pointer (for protocol processing)
        FORCE_INLINE bool prepend_data(size_t len) noexcept {
            if (UNLIKELY(len > data_offset)) return false;
            data_offset -= len;
            length += len;
            return true;
        }
        
        FORCE_INLINE bool trim_data(size_t len) noexcept {
            if (UNLIKELY(len > length)) return false;
            length -= len;
            return true;
        }
    };
    
    struct Config {
        const char* pci_device_id = "0000:01:00.0";
        uint16_t rx_queue_size = 4096;
        uint16_t tx_queue_size = 4096;
        uint16_t nb_rx_queues = 1;
        uint16_t nb_tx_queues = 1;
        uint16_t nb_rx_desc = 1024;       // RX descriptors per queue
        uint16_t nb_tx_desc = 1024;       // TX descriptors per queue
        bool enable_rss = false;
        bool enable_hw_timestamping = true;
        bool enable_checksum_offload = true;
        bool enable_lro = false;          // Large Receive Offload
        bool enable_tso = false;          // TCP Segmentation Offload
        uint32_t mtu = 1500;
        uint32_t rx_buffer_size = 2048;
        const char* driver_name = "igb_uio";
    };

private:
    std::atomic<bool> initialized_{false};
    Config config_;
    
    // Memory management
    using BufferPool = MemoryPool<sizeof(PacketBuffer), 16384>;
    std::unique_ptr<BufferPool> buffer_pool_;
    
    // Performance counters (separate cache lines)
    CACHE_ALIGNED std::atomic<uint64_t> rx_packets_{0};
    CACHE_ALIGNED std::atomic<uint64_t> rx_bytes_{0};
    CACHE_ALIGNED std::atomic<uint64_t> rx_drops_{0};
    CACHE_ALIGNED std::atomic<uint64_t> tx_packets_{0};
    CACHE_ALIGNED std::atomic<uint64_t> tx_bytes_{0};
    CACHE_ALIGNED std::atomic<uint64_t> tx_drops_{0};
    CACHE_ALIGNED std::atomic<uint64_t> rx_errors_{0};
    CACHE_ALIGNED std::atomic<uint64_t> tx_errors_{0};
    
    // Hardware-specific data (would be populated by DPDK)
    void* dpdk_port_info_ = nullptr;
    uint16_t port_id_ = 0;
    
    bool setup_dpdk_environment() noexcept;
    bool configure_port() noexcept;
    bool setup_queues() noexcept;
    void update_link_status() noexcept;

public:
    explicit KernelBypass(const Config& config = {}) : config_(config) {
        // Validate configuration
        if (config_.rx_queue_size == 0 || config_.tx_queue_size == 0) {
            throw std::invalid_argument("Queue sizes must be non-zero");
        }
        if (config_.nb_rx_desc == 0 || config_.nb_tx_desc == 0) {
            throw std::invalid_argument("Descriptor counts must be non-zero");
        }
        buffer_pool_ = std::make_unique<BufferPool>();
    }
    
    ~KernelBypass() { cleanup(); }
    
    // Non-copyable, non-movable for safety
    KernelBypass(const KernelBypass&) = delete;
    KernelBypass& operator=(const KernelBypass&) = delete;
    KernelBypass(KernelBypass&&) = delete;
    KernelBypass& operator=(KernelBypass&&) = delete;
    
    // Initialize DPDK environment
    bool initialize() noexcept;
    void cleanup() noexcept;
    
    // Packet reception (zero-copy, burst mode)
    FORCE_INLINE HOT_PATH uint16_t receive_packets(PacketBuffer** packets, uint16_t max_packets, ErrorCode* error = nullptr) noexcept {
        if (UNLIKELY(!initialized_.load(std::memory_order_acquire))) {
            if (error) *error = ErrorCode::NETWORK_ERROR;
            return 0;
        }
        if (error) *error = ErrorCode::SUCCESS;
        
        uint16_t received = 0;
        
        // In real DPDK implementation, this would call rte_eth_rx_burst()
        // For now, simulate receiving packets
        for (uint16_t i = 0; i < max_packets && i < 4; ++i) {
            PacketBuffer* pkt = allocate_buffer();
            if (UNLIKELY(!pkt)) break;
            
            // Simulate packet data
            pkt->length = 64; // Minimum Ethernet frame
            pkt->timestamp = std::chrono::steady_clock::now();
            pkt->port_id = port_id_;
            pkt->queue_id = 0;
            
            packets[i] = pkt;
            ++received;
        }
        
        if (LIKELY(received > 0)) {
            rx_packets_.fetch_add(received, std::memory_order_relaxed);
            // rx_bytes would be calculated from actual packet lengths
        }
        
        return received;
    }
    
    // Packet transmission (burst mode)
    FORCE_INLINE HOT_PATH uint16_t transmit_packets(PacketBuffer** packets, uint16_t nb_packets) noexcept {
        if (UNLIKELY(!initialized_.load(std::memory_order_acquire) || !packets)) {
            return 0;
        }
        
        uint16_t transmitted = 0;
        uint64_t total_bytes = 0;
        
        // In real DPDK implementation, this would call rte_eth_tx_burst()
        for (uint16_t i = 0; i < nb_packets; ++i) {
            if (UNLIKELY(!packets[i])) continue;
            
            // Simulate transmission
            total_bytes += packets[i]->length;
            ++transmitted;
            
            // Free the buffer after transmission
            free_buffer(packets[i]);
            packets[i] = nullptr;
        }
        
        if (LIKELY(transmitted > 0)) {
            tx_packets_.fetch_add(transmitted, std::memory_order_relaxed);
            tx_bytes_.fetch_add(total_bytes, std::memory_order_relaxed);
        }
        
        return transmitted;
    }
    
    // Buffer management
    FORCE_INLINE HOT_PATH PacketBuffer* allocate_buffer() noexcept {
        void* ptr = buffer_pool_->allocate();
        if (UNLIKELY(!ptr)) {
            rx_drops_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        
        return new(ptr) PacketBuffer();
    }
    
    FORCE_INLINE HOT_PATH void free_buffer(PacketBuffer* buffer) noexcept {
        if (LIKELY(buffer)) {
            buffer->~PacketBuffer();
            buffer_pool_->deallocate(buffer);
        }
    }
    
    // Batch buffer operations
    FORCE_INLINE uint16_t allocate_buffers(PacketBuffer** buffers, uint16_t count) noexcept {
        uint16_t allocated = 0;
        
        for (uint16_t i = 0; i < count; ++i) {
            buffers[i] = allocate_buffer();
            if (UNLIKELY(!buffers[i])) {
                break;
            }
            ++allocated;
        }
        
        return allocated;
    }
    
    FORCE_INLINE void free_buffers(PacketBuffer** buffers, uint16_t count) noexcept {
        for (uint16_t i = 0; i < count; ++i) {
            free_buffer(buffers[i]);
            buffers[i] = nullptr;
        }
    }
    
    // Hardware timestamping
    FORCE_INLINE HOT_PATH Timestamp get_hw_timestamp(const PacketBuffer* packet) const noexcept {
        if (LIKELY(packet && config_.enable_hw_timestamping)) {
            return packet->timestamp;
        }
        return std::chrono::steady_clock::now();
    }
    
    // Link and port management
    struct LinkStatus {
        bool link_up;
        uint32_t link_speed;    // Mbps
        bool full_duplex;
        bool autoneg;
    };
    
    LinkStatus get_link_status() const noexcept;
    bool set_promiscuous_mode(bool enable) noexcept;
    bool set_mtu(uint32_t mtu) noexcept;
    
    // Statistics and monitoring
    struct Stats {
        uint64_t rx_packets;
        uint64_t rx_bytes;
        uint64_t rx_drops;
        uint64_t rx_errors;
        uint64_t tx_packets;
        uint64_t tx_bytes;
        uint64_t tx_drops;
        uint64_t tx_errors;
        uint64_t buffer_pool_available;
        uint64_t buffer_pool_allocated;
    };
    
    Stats get_stats() const noexcept {
        return {
            rx_packets_.load(std::memory_order_relaxed),
            rx_bytes_.load(std::memory_order_relaxed),
            rx_drops_.load(std::memory_order_relaxed),
            rx_errors_.load(std::memory_order_relaxed),
            tx_packets_.load(std::memory_order_relaxed),
            tx_bytes_.load(std::memory_order_relaxed),
            tx_drops_.load(std::memory_order_relaxed),
            tx_errors_.load(std::memory_order_relaxed),
            buffer_pool_->available_count(),
            buffer_pool_->allocated_count()
        };
    }
    
    void reset_stats() noexcept {
        rx_packets_.store(0, std::memory_order_relaxed);
        rx_bytes_.store(0, std::memory_order_relaxed);
        rx_drops_.store(0, std::memory_order_relaxed);
        rx_errors_.store(0, std::memory_order_relaxed);
        tx_packets_.store(0, std::memory_order_relaxed);
        tx_bytes_.store(0, std::memory_order_relaxed);
        tx_drops_.store(0, std::memory_order_relaxed);
        tx_errors_.store(0, std::memory_order_relaxed);
    }
    
    FORCE_INLINE bool is_initialized() const noexcept { 
        return initialized_.load(std::memory_order_acquire); 
    }
    
    FORCE_INLINE uint16_t get_port_id() const noexcept { return port_id_; }
    
    // Buffer pool statistics
    FORCE_INLINE size_t available_buffers() const noexcept {
        return buffer_pool_->available_count();
    }
    
    FORCE_INLINE bool buffer_pool_exhausted() const noexcept {
        return buffer_pool_->available_count() == 0;
    }
};

// Raw socket implementation for systems without DPDK
class RawSocketBypass {
private:
    int raw_socket_ = -1;
    std::atomic<bool> initialized_{false};
    Config config_;
    
    // Statistics
    CACHE_ALIGNED std::atomic<uint64_t> rx_packets_{0};
    CACHE_ALIGNED std::atomic<uint64_t> rx_bytes_{0};
    CACHE_ALIGNED std::atomic<uint64_t> tx_packets_{0};
    CACHE_ALIGNED std::atomic<uint64_t> tx_bytes_{0};
    
    bool setup_raw_socket() noexcept;
    bool set_socket_options() noexcept;
    bool bind_to_interface() noexcept;

public:
    struct Config {
        const char* interface_name = "eth0";
        bool enable_promiscuous = false;
        size_t buffer_size = 64 * 1024 * 1024;
        int socket_priority = 7;          // Highest priority
        bool enable_timestamping = true;
        bool enable_packet_mmap = false;  // Use PACKET_MMAP for better performance
        uint32_t ring_size = 4096;        // Ring buffer size for PACKET_MMAP
    };
    
    explicit RawSocketBypass(const Config& config = {}) : config_(config) {}
    
    ~RawSocketBypass() { cleanup(); }
    
    // Non-copyable, non-movable
    RawSocketBypass(const RawSocketBypass&) = delete;
    RawSocketBypass& operator=(const RawSocketBypass&) = delete;
    RawSocketBypass(RawSocketBypass&&) = delete;
    RawSocketBypass& operator=(RawSocketBypass&&) = delete;
    
    bool initialize() noexcept;
    void cleanup() noexcept;
    
    // Raw packet I/O
    FORCE_INLINE HOT_PATH ssize_t receive_raw(char* buffer, size_t buffer_size) noexcept {
        if (UNLIKELY(!initialized_.load(std::memory_order_acquire) || raw_socket_ < 0)) {
            return -1;
        }
        
#ifdef __linux__
        ssize_t bytes = recv(raw_socket_, buffer, buffer_size, MSG_DONTWAIT);
        if (LIKELY(bytes > 0)) {
            rx_packets_.fetch_add(1, std::memory_order_relaxed);
            rx_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        }
        return bytes;
#else
        (void)buffer; (void)buffer_size;
        return -1;
#endif
    }
    
    FORCE_INLINE HOT_PATH ssize_t transmit_raw(const char* buffer, size_t length) noexcept {
        if (UNLIKELY(!initialized_.load(std::memory_order_acquire) || raw_socket_ < 0)) {
            return -1;
        }
        
#ifdef __linux__
        ssize_t bytes = send(raw_socket_, buffer, length, MSG_DONTWAIT);
        if (LIKELY(bytes > 0)) {
            tx_packets_.fetch_add(1, std::memory_order_relaxed);
            tx_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        }
        return bytes;
#else
        (void)buffer; (void)length;
        return -1;
#endif
    }
    
    // Receive with timestamp
    struct RawPacket {
        ssize_t length;
        Timestamp timestamp;
        char data[2048];
    };
    
    FORCE_INLINE ssize_t receive_with_timestamp(RawPacket& packet) noexcept {
        packet.timestamp = std::chrono::steady_clock::now();
        packet.length = receive_raw(packet.data, sizeof(packet.data));
        return packet.length;
    }
    
    // Statistics
    struct Stats {
        uint64_t rx_packets;
        uint64_t rx_bytes;
        uint64_t tx_packets;
        uint64_t tx_bytes;
    };
    
    Stats get_stats() const noexcept {
        return {
            rx_packets_.load(std::memory_order_relaxed),
            rx_bytes_.load(std::memory_order_relaxed),
            tx_packets_.load(std::memory_order_relaxed),
            tx_bytes_.load(std::memory_order_relaxed)
        };
    }
    
    void reset_stats() noexcept {
        rx_packets_.store(0, std::memory_order_relaxed);
        rx_bytes_.store(0, std::memory_order_relaxed);
        tx_packets_.store(0, std::memory_order_relaxed);
        tx_bytes_.store(0, std::memory_order_relaxed);
    }
    
    FORCE_INLINE bool is_initialized() const noexcept { 
        return initialized_.load(std::memory_order_acquire); 
    }
    
    // Socket information
    FORCE_INLINE int get_socket_fd() const noexcept { return raw_socket_; }
    
    // Advanced features
    bool set_cpu_affinity(int cpu_id) noexcept;
    bool enable_busy_polling(uint32_t budget) noexcept;
    bool set_socket_buffer_size(size_t rx_size, size_t tx_size) noexcept;
};

// Factory function for platform detection with better type safety
template<typename ConfigType>
auto create_network_interface(const ConfigType& config) {
#ifdef DPDK_AVAILABLE
    if constexpr (std::is_same_v<ConfigType, typename KernelBypass::Config>) {
        return std::make_unique<KernelBypass>(config);
    } else {
        typename KernelBypass::Config dpdk_config{};
        return std::make_unique<KernelBypass>(dpdk_config);
    }
#else
    if constexpr (std::is_same_v<ConfigType, typename RawSocketBypass::Config>) {
        return std::make_unique<RawSocketBypass>(config);
    } else {
        typename RawSocketBypass::Config raw_config{};
        return std::make_unique<RawSocketBypass>(raw_config);
    }
#endif
}

} // namespace hft