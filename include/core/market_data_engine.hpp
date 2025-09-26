#pragma once

#include "common/types.hpp"
#include "structures/lock_free_queue.hpp"
#include "structures/order_book.hpp"
#include "network/udp_receiver.hpp"
#include "fpga/fpga_interface.hpp"
#include "memory/object_pool.hpp"
#include "utils/high_precision_timer.hpp"
#include "utils/cpu_affinity.hpp"
#include "core/message_parser.hpp"
#include <unordered_map>
#include <thread>
#include <atomic>
#include <vector>

namespace hft {

class MarketDataEngine {
public:
    struct Config {
        // Network configuration
        UDPReceiver::Config network_config;
        
        // FPGA configuration
        FPGAInterface::FPGAConfig fpga_config;
        bool enable_fpga = true;
        
        // Processing configuration
        int processing_cpu = 1;
        int network_cpu = 2;
        size_t max_symbols = 10000;
        bool enable_order_book_processing = true;
        bool enable_latency_measurement = true;
        
        // Memory configuration
        size_t message_pool_size = 1000000;
        size_t order_pool_size = 10000000;
    };
    
    struct Statistics {
        uint64_t messages_received;
        uint64_t messages_processed;
        uint64_t parse_errors;
        uint64_t order_book_updates;
        LatencyStats::Stats processing_latency;
        LatencyStats::Stats end_to_end_latency;
        uint64_t symbols_active;
    };

private:
    Config config_;
    
    // Core components
    std::unique_ptr<UDPReceiver> network_receiver_;
    std::unique_ptr<FPGAInterface> fpga_interface_;
    std::unique_ptr<MessageParser> message_parser_;
    
    // Memory pools
    std::unique_ptr<ObjectPool<NetworkMessage>> message_pool_;
    std::unique_ptr<ObjectPool<Order>> order_pool_;
    
    // Order books per symbol
    std::unordered_map<SymbolId, std::unique_ptr<OrderBook>> order_books_;
    
    // Processing threads
    std::thread processing_thread_;
    std::atomic<bool> running_{false};
    
    // Performance monitoring
    LatencyStats processing_latency_stats_;
    LatencyStats end_to_end_latency_stats_;
    
    CACHE_ALIGNED std::atomic<uint64_t> messages_received_{0};
    CACHE_ALIGNED std::atomic<uint64_t> messages_processed_{0};
    CACHE_ALIGNED std::atomic<uint64_t> parse_errors_{0};
    CACHE_ALIGNED std::atomic<uint64_t> order_book_updates_{0};
    
    // Message processing pipeline
    void processing_loop() noexcept;
    FORCE_INLINE bool process_message(NetworkMessage* msg) noexcept;
    FORCE_INLINE bool update_order_book(const ParsedMessage& parsed_msg) noexcept;
    FORCE_INLINE OrderBook* get_or_create_order_book(SymbolId symbol_id) noexcept;
    
    // Callbacks for market data events
    std::function<void(SymbolId, const PriceLevel*, const PriceLevel*)> on_best_quote_change_;
    std::function<void(SymbolId, Price, Quantity)> on_trade_;
    std::function<void(const Statistics&)> on_statistics_update_;

public:
    explicit MarketDataEngine(const Config& config);
    ~MarketDataEngine();
    
    // Non-copyable, non-movable
    MarketDataEngine(const MarketDataEngine&) = delete;
    MarketDataEngine& operator=(const MarketDataEngine&) = delete;
    MarketDataEngine(MarketDataEngine&&) = delete;
    MarketDataEngine& operator=(MarketDataEngine&&) = delete;
    
    // Lifecycle management
    bool initialize() noexcept;
    bool start() noexcept;
    void stop() noexcept;
    void shutdown() noexcept;
    
    // Market data access
    FORCE_INLINE const OrderBook* get_order_book(SymbolId symbol_id) const noexcept {
        auto it = order_books_.find(symbol_id);
        return it != order_books_.end() ? it->second.get() : nullptr;
    }
    
    FORCE_INLINE Price get_best_bid(SymbolId symbol_id) const noexcept {
        const auto* book = get_order_book(symbol_id);
        const auto* best_bid = book ? book->get_best_bid() : nullptr;
        return best_bid ? best_bid->price : 0;
    }
    
    FORCE_INLINE Price get_best_ask(SymbolId symbol_id) const noexcept {
        const auto* book = get_order_book(symbol_id);
        const auto* best_ask = book ? book->get_best_ask() : nullptr;
        return best_ask ? best_ask->price : 0;
    }
    
    FORCE_INLINE Price get_mid_price(SymbolId symbol_id) const noexcept {
        const auto* book = get_order_book(symbol_id);
        return book ? book->get_mid_price() : 0;
    }
    
    FORCE_INLINE Price get_spread(SymbolId symbol_id) const noexcept {
        const auto* book = get_order_book(symbol_id);
        return book ? book->get_spread() : 0;
    }
    
    // Event callbacks
    void set_quote_callback(std::function<void(SymbolId, const PriceLevel*, const PriceLevel*)> callback) {
        on_best_quote_change_ = std::move(callback);
    }
    
    void set_trade_callback(std::function<void(SymbolId, Price, Quantity)> callback) {
        on_trade_ = std::move(callback);
    }
    
    void set_statistics_callback(std::function<void(const Statistics&)> callback) {
        on_statistics_update_ = std::move(callback);
    }
    
    // Statistics and monitoring
    Statistics get_statistics() const noexcept;
    void reset_statistics() noexcept;
    
    // Health monitoring
    bool is_healthy() const noexcept;
    Duration get_average_processing_latency() const noexcept;
    Duration get_average_end_to_end_latency() const noexcept;
    
    // Symbol management
    bool add_symbol(SymbolId symbol_id) noexcept;
    bool remove_symbol(SymbolId symbol_id) noexcept;
    std::vector<SymbolId> get_active_symbols() const noexcept;
    
    bool is_running() const noexcept { return running_.load(std::memory_order_acquire); }
};

// Factory function for easy configuration
std::unique_ptr<MarketDataEngine> create_hft_engine(const MarketDataEngine::Config& config = {});

} // namespace hft
