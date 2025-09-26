#pragma once

#include "common/types.hpp"
#include <cstdint>

namespace hft {

// DPDK-style packet processing interface
class KernelBypass {
public:
    struct PacketBuffer {
        static constexpr size_t MAX_PACKET_SIZE = 2048;
        
        uint16_t length;
        uint16_t data_offset;
        Timestamp timestamp;
        alignas(16) char data[MAX_PACKET_SIZE];
        
        FORCE_INLINE char* get_data() noexcept {
            return data + data_offset;
        }
        
        FORCE_INLINE const char* get_data() const noexcept {
            return data + data_offset;
        }
    };
    
    struct Config {
        const char* pci_device_id = "0000:01:00.0";
        uint16_t rx_queue_size = 4096;
        uint16_t tx_queue_size = 4096;
        uint16_t nb_rx_queues = 1;
        uint16_t nb_tx_queues = 1;
        bool enable_rss = false;
        bool enable_hw_timestamping = true;
    };

private:
    bool initialized_ = false;
    Config config_;
    
    // Simulated packet buffers (in real implementation, these would be DMA buffers)
    static constexpr size_t BUFFER_POOL_SIZE = 8192;
    PacketBuffer buffer_pool_[BUFFER_POOL_SIZE];
    std::atomic<size_t> buffer_index_{0};

public:
    explicit KernelBypass(const Config& config) : config_(config) {}
    ~KernelBypass() { cleanup(); }
    
    // Initialize DPDK environment
    bool initialize() noexcept;
    void cleanup() noexcept;
    
    // Packet reception (zero-copy)
    FORCE_INLINE uint16_t receive_packets(PacketBuffer** packets, uint16_t max_packets) noexcept;
    
    // Packet transmission
    FORCE_INLINE uint16_t transmit_packets(PacketBuffer** packets, uint16_t nb_packets) noexcept;
    
    // Buffer management
    FORCE_INLINE PacketBuffer* allocate_buffer() noexcept;
    FORCE_INLINE void free_buffer(PacketBuffer* buffer) noexcept;
    
    // Hardware timestamping
    FORCE_INLINE Timestamp get_hw_timestamp(const PacketBuffer* packet) const noexcept;
    
    // Statistics and monitoring
    struct Stats {
        uint64_t rx_packets;
        uint64_t rx_bytes;
        uint64_t rx_drops;
        uint64_t tx_packets;
        uint64_t tx_bytes;
        uint64_t tx_drops;
    };
    
    Stats get_stats() const noexcept;
    void reset_stats() noexcept;
    
    bool is_initialized() const noexcept { return initialized_; }
};

// Raw socket implementation for systems without DPDK
class RawSocketBypass {
private:
    int raw_socket_;
    bool initialized_ = false;
    
public:
    struct Config {
        const char* interface_name = "eth0";
        bool enable_promiscuous = false;
        size_t buffer_size = 64 * 1024 * 1024;
    };
    
    explicit RawSocketBypass(const Config& config);
    ~RawSocketBypass();
    
    bool initialize() noexcept;
    void cleanup() noexcept;
    
    FORCE_INLINE ssize_t receive_raw(char* buffer, size_t buffer_size) noexcept;
    FORCE_INLINE ssize_t transmit_raw(const char* buffer, size_t length) noexcept;
    
    bool is_initialized() const noexcept { return initialized_; }
};

} // namespace hft
