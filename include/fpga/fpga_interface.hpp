#pragma once

#include "common/types.hpp"
#include <atomic>
#include <memory>
#include <cstring>

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
        uint32_t max_symbols = 16384;
        uint32_t max_orders_per_symbol = 10000;
        Duration timeout = std::chrono::microseconds(100);
    };
    
    // Hardware command types for FPGA communication
    enum class HWCommand : uint8_t {
        INVALID = 0,
        ADD_ORDER = 1,
        MODIFY_ORDER = 2,
        CANCEL_ORDER = 3,
        GET_QUOTE = 4,
        RISK_CHECK = 5,
        RESET = 6,
        HEARTBEAT = 7
    };
    
    // Hardware-accelerated message processing
    struct alignas(64) HWMessage {
        // Header (cache line aligned for optimal DMA)
        uint64_t sequence_number;
        Timestamp hw_timestamp;
        HWCommand command;
        MessageType type;
        SymbolId symbol_id;
        ErrorCode error_code;
        uint16_t padding1;
        
        // Payload (union for different message types)
        union {
            struct {
                Price price;
                Quantity quantity;
                Side side;
                OrderId order_id;
                uint32_t padding2;
            } order_data;
            
            struct {
                Price price;
                Quantity quantity;
                uint64_t padding3;
            } trade_data;
            
            struct {
                Price bid_price;
                Quantity bid_quantity;
                Price ask_price;
                Quantity ask_quantity;
            } quote_data;
            
            struct {
                SymbolId symbol_id;
                Side side;
                Quantity quantity;
                Price price;
                bool result;
                uint8_t padding4[7];
            } risk_data;
        };
        
        HWMessage() noexcept {
            std::memset(this, 0, sizeof(HWMessage));
            type = MessageType::INVALID;
            command = HWCommand::INVALID;
        }
        
        // Factory methods for type safety
        static HWMessage create_add_order(SymbolId symbol, OrderId order_id, 
                                         Price price, Quantity quantity, Side side) noexcept {
            HWMessage msg;
            msg.command = HWCommand::ADD_ORDER;
            msg.type = MessageType::ORDER_ADD;
            msg.symbol_id = symbol;
            msg.order_data.order_id = order_id;
            msg.order_data.price = price;
            msg.order_data.quantity = quantity;
            msg.order_data.side = side;
            return msg;
        }
        
        static HWMessage create_modify_order(OrderId order_id, Quantity new_quantity) noexcept {
            HWMessage msg;
            msg.command = HWCommand::MODIFY_ORDER;
            msg.type = MessageType::ORDER_MODIFY;
            msg.order_data.order_id = order_id;
            msg.order_data.quantity = new_quantity;
            return msg;
        }
        
        static HWMessage create_cancel_order(OrderId order_id) noexcept {
            HWMessage msg;
            msg.command = HWCommand::CANCEL_ORDER;
            msg.type = MessageType::ORDER_DELETE;
            msg.order_data.order_id = order_id;
            return msg;
        }
    };
    
    static_assert(sizeof(HWMessage) == 64, "HWMessage must be exactly 64 bytes for optimal DMA");
    
    // Lock-free DMA ring buffer for FPGA communication
    template<size_t Size>
    struct DMABuffer {
        static_assert((Size & (Size - 1)) == 0, "Buffer size must be power of 2");
        static constexpr size_t BUFFER_SIZE = Size;
        static constexpr size_t MASK = Size - 1;
        
        CACHE_ALIGNED std::atomic<uint32_t> head{0};
        CACHE_ALIGNED std::atomic<uint32_t> tail{0};
        CACHE_ALIGNED HWMessage messages[BUFFER_SIZE];
        
        FORCE_INLINE HOT_PATH bool is_empty() const noexcept {
            return head.load(std::memory_order_acquire) == 
                   tail.load(std::memory_order_acquire);
        }
        
        FORCE_INLINE HOT_PATH bool is_full() const noexcept {
            return ((tail.load(std::memory_order_relaxed) + 1) & MASK) == 
                   head.load(std::memory_order_acquire);
        }
        
        FORCE_INLINE HOT_PATH bool try_push(const HWMessage& msg) noexcept {
            const uint32_t current_tail = tail.load(std::memory_order_relaxed);
            const uint32_t next_tail = (current_tail + 1) & MASK;
            
            if (UNLIKELY(next_tail == head.load(std::memory_order_acquire))) {
                return false; // Full
            }
            
            messages[current_tail] = msg;
            tail.store(next_tail, std::memory_order_release);
            return true;
        }
        
        FORCE_INLINE HOT_PATH bool try_pop(HWMessage& msg) noexcept {
            const uint32_t current_head = head.load(std::memory_order_relaxed);
            
            if (UNLIKELY(current_head == tail.load(std::memory_order_acquire))) {
                return false; // Empty
            }
            
            msg = messages[current_head];
            head.store((current_head + 1) & MASK, std::memory_order_release);
            return true;
        }
        
        FORCE_INLINE size_t size() const noexcept {
            const uint32_t current_tail = tail.load(std::memory_order_acquire);
            const uint32_t current_head = head.load(std::memory_order_acquire);
            return (current_tail - current_head) & MASK;
        }
    };

private:
    using RxBuffer = DMABuffer<4096>;
    using TxBuffer = DMABuffer<4096>;
    
    int fpga_fd_ = -1;
    void* dma_memory_ = nullptr;
    std::unique_ptr<RxBuffer> rx_buffer_;
    std::unique_ptr<TxBuffer> tx_buffer_;
    FPGAConfig config_;
    std::atomic<bool> initialized_{false};
    std::atomic<uint64_t> next_sequence_{1};
    
    // Performance counters (separate cache lines)
    CACHE_ALIGNED std::atomic<uint64_t> messages_processed_{0};
    CACHE_ALIGNED std::atomic<uint64_t> hardware_errors_{0};
    CACHE_ALIGNED std::atomic<uint64_t> dma_transfers_{0};
    CACHE_ALIGNED std::atomic<uint64_t> timeouts_{0};
    CACHE_ALIGNED std::atomic<uint64_t> last_heartbeat_{0};
    
    // Hardware state monitoring
    CACHE_ALIGNED std::atomic<uint32_t> fpga_temperature_{0};
    CACHE_ALIGNED std::atomic<uint32_t> fpga_utilization_{0};
    CACHE_ALIGNED std::atomic<bool> hardware_healthy_{false};
    
    bool setup_dma_buffers() noexcept;
    void cleanup_dma_buffers() noexcept;
    bool program_fpga() noexcept;
    bool wait_for_fpga_ready(Duration timeout = std::chrono::milliseconds(1000)) noexcept;
    void update_hardware_stats() noexcept;
    
    FORCE_INLINE bool send_hw_message(const HWMessage& msg) noexcept {
        if (UNLIKELY(!initialized_.load(std::memory_order_acquire))) {
            return false;
        }
        
        // Add sequence number
        HWMessage seq_msg = msg;
        seq_msg.sequence_number = next_sequence_.fetch_add(1, std::memory_order_relaxed);
        
        bool success = tx_buffer_->try_push(seq_msg);
        if (LIKELY(success)) {
            dma_transfers_.fetch_add(1, std::memory_order_relaxed);
        }
        return success;
    }

public:
    explicit FPGAInterface(const FPGAConfig& config = {}) : config_(config) {
        rx_buffer_ = std::make_unique<RxBuffer>();
        tx_buffer_ = std::make_unique<TxBuffer>();
    }
    
    ~FPGAInterface() { cleanup(); }
    
    // Non-copyable, non-movable for safety
    FPGAInterface(const FPGAInterface&) = delete;
    FPGAInterface& operator=(const FPGAInterface&) = delete;
    FPGAInterface(FPGAInterface&&) = delete;
    FPGAInterface& operator=(FPGAInterface&&) = delete;
    
    // Initialization and cleanup
    bool initialize() noexcept;
    void cleanup() noexcept;
    
    // Message processing
    FORCE_INLINE HOT_PATH bool send_message_to_fpga(const HWMessage& msg) noexcept {
        bool success = send_hw_message(msg);
        if (LIKELY(success)) {
            messages_processed_.fetch_add(1, std::memory_order_relaxed);
        }
        return success;
    }
    
    FORCE_INLINE HOT_PATH bool receive_from_fpga(HWMessage& msg) noexcept {
        if (UNLIKELY(!initialized_.load(std::memory_order_acquire))) {
            return false;
        }
        
        return rx_buffer_->try_pop(msg);
    }
    
    // Batch operations for better throughput
    FORCE_INLINE size_t send_batch(const HWMessage* messages, size_t count) noexcept {
        size_t sent = 0;
        for (size_t i = 0; i < count; ++i) {
            if (!send_hw_message(messages[i])) {
                break;
            }
            ++sent;
        }
        messages_processed_.fetch_add(sent, std::memory_order_relaxed);
        return sent;
    }
    
    FORCE_INLINE size_t receive_batch(HWMessage* messages, size_t max_count) noexcept {
        if (UNLIKELY(!initialized_.load(std::memory_order_acquire))) {
            return 0;
        }
        
        size_t received = 0;
        for (size_t i = 0; i < max_count; ++i) {
            if (!rx_buffer_->try_pop(messages[i])) {
                break;
            }
            ++received;
        }
        return received;
    }
    
    // Hardware-accelerated order book operations
    FORCE_INLINE HOT_PATH bool add_order_hw(SymbolId symbol, OrderId order_id, 
                                            Price price, Quantity quantity, Side side) noexcept {
        auto msg = HWMessage::create_add_order(symbol, order_id, price, quantity, side);
        return send_hw_message(msg);
    }
    
    FORCE_INLINE HOT_PATH bool modify_order_hw(OrderId order_id, Quantity new_quantity) noexcept {
        auto msg = HWMessage::create_modify_order(order_id, new_quantity);
        return send_hw_message(msg);
    }
    
    FORCE_INLINE HOT_PATH bool cancel_order_hw(OrderId order_id) noexcept {
        auto msg = HWMessage::create_cancel_order(order_id);
        return send_hw_message(msg);
    }
    
    // Get best bid/ask from FPGA order book
    struct BestQuote {
        Price bid_price;
        Quantity bid_quantity;
        Price ask_price;
        Quantity ask_quantity;
        Timestamp timestamp;
        bool valid;
        
        BestQuote() : bid_price(0), bid_quantity(0), ask_price(0), ask_quantity(0)
                    , timestamp{}, valid(false) {}
    };
    
    FORCE_INLINE bool get_best_quote_hw(SymbolId symbol, BestQuote& quote) noexcept {
        HWMessage msg;
        msg.command = HWCommand::GET_QUOTE;
        msg.symbol_id = symbol;
        
        if (!send_hw_message(msg)) {
            return false;
        }
        
        // In a real implementation, this would wait for response
        // For now, return placeholder
        quote = BestQuote{};
        return true;
    }
    
    // Risk management acceleration
    FORCE_INLINE bool check_risk_hw(SymbolId symbol, Side side, 
                                   Quantity quantity, Price price) noexcept {
        HWMessage msg;
        msg.command = HWCommand::RISK_CHECK;
        msg.symbol_id = symbol;
        msg.risk_data.symbol_id = symbol;
        msg.risk_data.side = side;
        msg.risk_data.quantity = quantity;
        msg.risk_data.price = price;
        
        return send_hw_message(msg);
    }
    
    // Latency measurement using FPGA timestamps
    FORCE_INLINE Duration get_processing_latency() const noexcept {
        // Would read from FPGA registers in real implementation
        return std::chrono::nanoseconds(100); // Placeholder
    }
    
    // Health monitoring and heartbeat
    FORCE_INLINE bool send_heartbeat() noexcept {
        HWMessage msg;
        msg.command = HWCommand::HEARTBEAT;
        msg.type = MessageType::HEARTBEAT;
        
        bool success = send_hw_message(msg);
        if (success) {
            last_heartbeat_.store(
                std::chrono::steady_clock::now().time_since_epoch().count(),
                std::memory_order_relaxed);
        }
        return success;
    }
    
    // Statistics
    struct FPGAStats {
        uint64_t messages_processed;
        uint64_t hardware_errors;
        uint64_t dma_transfers;
        uint64_t timeouts;
        uint32_t fpga_temperature; // Celsius
        uint32_t fpga_utilization; // Percentage
        Duration avg_processing_time;
        size_t tx_buffer_size;
        size_t rx_buffer_size;
        bool hardware_healthy;
    };
    
    FPGAStats get_stats() const noexcept {
        update_hardware_stats();
        return {
            messages_processed_.load(std::memory_order_relaxed),
            hardware_errors_.load(std::memory_order_relaxed),
            dma_transfers_.load(std::memory_order_relaxed),
            timeouts_.load(std::memory_order_relaxed),
            fpga_temperature_.load(std::memory_order_relaxed),
            fpga_utilization_.load(std::memory_order_relaxed),
            get_processing_latency(),
            tx_buffer_->size(),
            rx_buffer_->size(),
            hardware_healthy_.load(std::memory_order_relaxed)
        };
    }
    
    void reset_stats() noexcept {
        messages_processed_.store(0, std::memory_order_relaxed);
        hardware_errors_.store(0, std::memory_order_relaxed);
        dma_transfers_.store(0, std::memory_order_relaxed);
        timeouts_.store(0, std::memory_order_relaxed);
    }
    
    // Health monitoring
    FORCE_INLINE bool is_healthy() const noexcept {
        return initialized_.load(std::memory_order_acquire) && 
               hardware_healthy_.load(std::memory_order_relaxed);
    }
    
    FORCE_INLINE uint32_t get_temperature() const noexcept {
        return fpga_temperature_.load(std::memory_order_relaxed);
    }
    
    FORCE_INLINE uint32_t get_utilization() const noexcept {
        return fpga_utilization_.load(std::memory_order_relaxed);
    }
    
    FORCE_INLINE bool is_initialized() const noexcept { 
        return initialized_.load(std::memory_order_acquire); 
    }
    
    // Buffer status
    FORCE_INLINE bool tx_buffer_full() const noexcept {
        return tx_buffer_->is_full();
    }
    
    FORCE_INLINE bool rx_buffer_empty() const noexcept {
        return rx_buffer_->is_empty();
    }
    
    FORCE_INLINE size_t pending_tx_messages() const noexcept {
        return tx_buffer_->size();
    }
    
    FORCE_INLINE size_t pending_rx_messages() const noexcept {
        return rx_buffer_->size();
    }
};

// Software fallback for systems without FPGA
class SoftwareFallback {
private:
    std::atomic<uint64_t> messages_processed_{0};
    std::atomic<bool> initialized_{false};
    
public:
    using HWMessage = FPGAInterface::HWMessage;
    using BestQuote = FPGAInterface::BestQuote;
    using FPGAConfig = FPGAInterface::FPGAConfig;
    using FPGAStats = FPGAInterface::FPGAStats;
    
    explicit SoftwareFallback(const FPGAConfig& config = {}) { (void)config; }
    
    bool initialize() noexcept { 
        initialized_.store(true, std::memory_order_release);
        return true; 
    }
    
    void cleanup() noexcept {
        initialized_.store(false, std::memory_order_release);
    }
    
    FORCE_INLINE HOT_PATH bool send_message_to_fpga(const HWMessage& msg) noexcept {
        messages_processed_.fetch_add(1, std::memory_order_relaxed);
        // Software processing would go here
        (void)msg;
        return true;
    }
    
    FORCE_INLINE HOT_PATH bool receive_from_fpga(HWMessage& msg) noexcept {
        // No messages in software fallback
        (void)msg;
        return false;
    }
    
    FORCE_INLINE bool get_best_quote_hw(SymbolId symbol, BestQuote& quote) noexcept {
        (void)symbol;
        quote = BestQuote{};
        return false;
    }
    
    FORCE_INLINE bool add_order_hw(SymbolId symbol, OrderId order_id, 
                                  Price price, Quantity quantity, Side side) noexcept {
        (void)symbol; (void)order_id; (void)price; (void)quantity; (void)side;
        return true; // Simulate success
    }
    
    FORCE_INLINE bool modify_order_hw(OrderId order_id, Quantity new_quantity) noexcept {
        (void)order_id; (void)new_quantity;
        return true;
    }
    
    FORCE_INLINE bool cancel_order_hw(OrderId order_id) noexcept {
        (void)order_id;
        return true;
    }
    
    FPGAStats get_stats() const noexcept {
        return {
            messages_processed_.load(std::memory_order_relaxed),
            0, 0, 0, 25, 0, std::chrono::nanoseconds(0), 0, 0, true
        };
    }
    
    void reset_stats() noexcept {
        messages_processed_.store(0, std::memory_order_relaxed);
    }
    
    bool is_initialized() const noexcept { 
        return initialized_.load(std::memory_order_acquire); 
    }
    
    bool is_healthy() const noexcept { return true; }
    uint32_t get_temperature() const noexcept { return 25; } // Room temp
    uint32_t get_utilization() const noexcept { return 0; }
};

} // namespace hft