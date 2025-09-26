#pragma once

#include "common/types.hpp"
#include <atomic>
#include <memory>

namespace hft {

// FPGA accelerated market data processing
class FPGAInterface {
public:
    struct FPGAConfig {
        const char* device_path = "/dev/fpga0";
        size_t dma_buffer_size = 64 * 1024 * 1024; // 64MB
        uint32_t clock_frequency = 400000000; // 400MHz
        bool enable_hardware_timestamping = true;
        bool enable_order_book_acceleration = true;
        bool enable_risk_checks = true;
    };
    
    // Hardware-accelerated message processing
    struct HWMessage {
        uint64_t sequence_number;
        Timestamp hw_timestamp;
        MessageType type;
        SymbolId symbol_id;
        union {
            struct {
                Price price;
                Quantity quantity;
                Side side;
                OrderId order_id;
            } order_data;
            
            struct {
                Price price;
                Quantity quantity;
            } trade_data;
            
            struct {
                Price bid_price;
                Quantity bid_quantity;
                Price ask_price;
                Quantity ask_quantity;
            } quote_data;
        };
        
        HWMessage() : sequence_number(0), hw_timestamp{}, type(MessageType::HEARTBEAT)
                    , symbol_id(0) {}
    };
    
    // DMA ring buffer for FPGA communication
    struct DMABuffer {
        static constexpr size_t BUFFER_SIZE = 4096;
        
        CACHE_ALIGNED std::atomic<uint32_t> head{0};
        CACHE_ALIGNED std::atomic<uint32_t> tail{0};
        CACHE_ALIGNED HWMessage messages[BUFFER_SIZE];
        
        FORCE_INLINE bool is_empty() const noexcept {
            return head.load(std::memory_order_acquire) == 
                   tail.load(std::memory_order_acquire);
        }
        
        FORCE_INLINE bool is_full() const noexcept {
            return ((tail.load(std::memory_order_acquire) + 1) % BUFFER_SIZE) == 
                   head.load(std::memory_order_acquire);
        }
    };

private:
    int fpga_fd_ = -1;
    void* dma_memory_ = nullptr;
    DMABuffer* rx_buffer_ = nullptr;
    DMABuffer* tx_buffer_ = nullptr;
    FPGAConfig config_;
    bool initialized_ = false;
    
    // Performance counters
    CACHE_ALIGNED std::atomic<uint64_t> messages_processed_{0};
    CACHE_ALIGNED std::atomic<uint64_t> hardware_errors_{0};
    CACHE_ALIGNED std::atomic<uint64_t> dma_transfers_{0};
    
    bool setup_dma_buffers() noexcept;
    void cleanup_dma_buffers() noexcept;
    bool program_fpga() noexcept;

public:
    explicit FPGAInterface(const FPGAConfig& config) : config_(config) {}
    ~FPGAInterface() { cleanup(); }
    
    // Initialization and cleanup
    bool initialize() noexcept;
    void cleanup() noexcept;
    
    // Message processing
    FORCE_INLINE bool send_message_to_fpga(const HWMessage& msg) noexcept;
    FORCE_INLINE HWMessage* receive_from_fpga() noexcept;
    
    // Hardware-accelerated order book operations
    FORCE_INLINE bool add_order_hw(SymbolId symbol, OrderId order_id, 
                                  Price price, Quantity quantity, Side side) noexcept;
    FORCE_INLINE bool modify_order_hw(OrderId order_id, Quantity new_quantity) noexcept;
    FORCE_INLINE bool cancel_order_hw(OrderId order_id) noexcept;
    
    // Get best bid/ask from FPGA order book
    struct BestQuote {
        Price bid_price;
        Quantity bid_quantity;
        Price ask_price;
        Quantity ask_quantity;
        bool valid;
    };
    
    FORCE_INLINE BestQuote get_best_quote_hw(SymbolId symbol) const noexcept;
    
    // Risk management acceleration
    FORCE_INLINE bool check_risk_hw(SymbolId symbol, Side side, 
                                   Quantity quantity, Price price) const noexcept;
    
    // Latency measurement using FPGA timestamps
    FORCE_INLINE Duration get_processing_latency() const noexcept;
    
    // Statistics
    struct FPGAStats {
        uint64_t messages_processed;
        uint64_t hardware_errors;
        uint64_t dma_transfers;
        uint32_t fpga_temperature; // Celsius
        uint32_t fpga_utilization; // Percentage
        Duration avg_processing_time;
    };
    
    FPGAStats get_stats() const noexcept;
    void reset_stats() noexcept;
    
    // Health monitoring
    bool is_healthy() const noexcept;
    uint32_t get_temperature() const noexcept;
    uint32_t get_utilization() const noexcept;
    
    bool is_initialized() const noexcept { return initialized_; }
};

// Software fallback for systems without FPGA
class SoftwareFallback {
private:
    std::atomic<uint64_t> messages_processed_{0};
    
public:
    using HWMessage = FPGAInterface::HWMessage;
    using BestQuote = FPGAInterface::BestQuote;
    
    bool initialize() noexcept { return true; }
    void cleanup() noexcept {}
    
    FORCE_INLINE bool send_message_to_fpga(const HWMessage& msg) noexcept {
        messages_processed_.fetch_add(1, std::memory_order_relaxed);
        // Software processing would go here
        (void)msg;
        return true;
    }
    
    FORCE_INLINE HWMessage* receive_from_fpga() noexcept {
        return nullptr; // No messages in software fallback
    }
    
    FORCE_INLINE BestQuote get_best_quote_hw(SymbolId symbol) const noexcept {
        (void)symbol;
        return {0, 0, 0, 0, false};
    }
    
    bool is_initialized() const noexcept { return true; }
    bool is_healthy() const noexcept { return true; }
};

} // namespace hft
