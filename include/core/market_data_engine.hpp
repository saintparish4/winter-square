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
#include <memory>
#include <functional>

namespace hft {

    class MarketDataEngine {
        public:
            struct Config {
                // Network configuration
                UDPReceiver::Config network_config;
                
                // FPGA configuration
                FPGAInterface::FPGAConfig fpga_config;
                bool enable_fpga = false;  // Default off for safety
                
                // Processing configuration
                int processing_cpu = 1;
                int network_cpu = 2;
                size_t max_symbols = 10000;
                bool enable_order_book_processing = true;
                bool enable_latency_measurement = true;
                
                // Memory configuration
                size_t message_pool_size = 1000000;
                size_t order_pool_size = 10000000;
                
                // Performance tuning
                size_t batch_size = 32;                     // Messages to process per batch
                Duration statistics_interval = std::chrono::seconds(1);
                bool enable_symbol_preallocation = true;   // Pre-allocate order books
                std::vector<SymbolId> preload_symbols;     // Symbols to preload
                
                // Validation
                bool is_valid() const noexcept {
                    return processing_cpu >= 0 && 
                           network_cpu >= 0 &&
                           max_symbols > 0 &&
                           message_pool_size > 0 &&
                           order_pool_size > 0 &&
                           batch_size > 0 &&
                           batch_size <= 1024;
                }
            };
            
            struct Statistics {
                uint64_t messages_received;
                uint64_t messages_processed;
                uint64_t messages_dropped;
                uint64_t parse_errors;
                uint64_t order_book_updates;
                uint64_t symbols_active;
                uint64_t pool_exhaustion_count;
                LatencyStats::Stats processing_latency;
                LatencyStats::Stats end_to_end_latency;
                Duration uptime;
                Timestamp last_update;
                
                // Component health
                bool network_healthy;
                bool fpga_healthy;
                bool processing_healthy;
            };
            
            enum class EngineState : uint8_t {
                UNINITIALIZED = 0,
                INITIALIZING = 1,
                INITIALIZED = 2,
                STARTING = 3,
                RUNNING = 4,
                STOPPING = 5,
                STOPPED = 6,
                ERROR = 7
            };
        
        private:
            Config config_;
            std::atomic<EngineState> state_{EngineState::UNINITIALIZED};
            
            // Core components
            std::unique_ptr<UDPReceiver> network_receiver_;
            std::unique_ptr<FPGAInterface> fpga_interface_;
            std::unique_ptr<MessageParser> message_parser_;
            
            // Memory pools
            std::unique_ptr<ObjectPool<NetworkMessage>> message_pool_;
            std::unique_ptr<ObjectPool<Order>> order_pool_;
            
            // Order books per symbol - using pointer for lock-free updates
            std::unordered_map<SymbolId, std::unique_ptr<OrderBook>> order_books_;
            std::atomic<size_t> active_symbols_{0};
            
            // Processing threads
            std::thread processing_thread_;
            std::thread statistics_thread_;
            std::atomic<bool> running_{false};
            
            // Performance monitoring
            LatencyStats processing_latency_stats_;
            LatencyStats end_to_end_latency_stats_;
            
            // Performance counters (separate cache lines)
            CACHE_ALIGNED std::atomic<uint64_t> messages_received_{0};
            CACHE_ALIGNED std::atomic<uint64_t> messages_processed_{0};
            CACHE_ALIGNED std::atomic<uint64_t> messages_dropped_{0};
            CACHE_ALIGNED std::atomic<uint64_t> parse_errors_{0};
            CACHE_ALIGNED std::atomic<uint64_t> order_book_updates_{0};
            CACHE_ALIGNED std::atomic<uint64_t> pool_exhaustion_count_{0};
            CACHE_ALIGNED std::atomic<uint64_t> callback_errors_{0};
            
            // Timing
            Timestamp start_time_;
            CACHE_ALIGNED std::atomic<uint64_t> last_message_time_{0};
            
            // Message processing pipeline
            void processing_loop() noexcept;
            void statistics_loop() noexcept;
            
            FORCE_INLINE HOT_PATH bool process_message(NetworkMessage* msg) noexcept;
            FORCE_INLINE HOT_PATH bool process_message_batch(NetworkMessage** messages, size_t count) noexcept;
            FORCE_INLINE HOT_PATH bool update_order_book(const ParsedMessage& parsed_msg) noexcept;
            FORCE_INLINE OrderBook* get_or_create_order_book(SymbolId symbol_id) noexcept;
            
            // Callbacks for market data events (with error handling)
            std::function<void(SymbolId, const PriceLevel*, const PriceLevel*)> on_best_quote_change_;
            std::function<void(SymbolId, Price, Quantity)> on_trade_;
            std::function<void(const Statistics&)> on_statistics_update_;
            std::function<void(ErrorCode, const char*)> on_error_;
            
            // Safe callback invocation with exception handling
            template<typename Func, typename... Args>
            FORCE_INLINE void safe_callback(Func& callback, Args&&... args) noexcept {
                if (callback) {
                    try {
                        callback(std::forward<Args>(args)...);
                    } catch (...) {
                        callback_errors_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            
            // Internal helpers
            bool initialize_components() noexcept;
            bool validate_configuration() const noexcept;
            void cleanup_resources() noexcept;
            void update_statistics() noexcept;
        
        public:
            explicit MarketDataEngine(const Config& config = {}) : config_(config) {
                if (!config_.is_valid()) {
                    state_.store(EngineState::ERROR, std::memory_order_release);
                }
            }
            
            ~MarketDataEngine() {
                shutdown();
            }
            
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
            
            // State management
            FORCE_INLINE EngineState get_state() const noexcept {
                return state_.load(std::memory_order_acquire);
            }
            
            FORCE_INLINE bool is_running() const noexcept { 
                return get_state() == EngineState::RUNNING;
            }
            
            FORCE_INLINE bool is_initialized() const noexcept {
                auto state = get_state();
                return state == EngineState::INITIALIZED || 
                       state == EngineState::RUNNING;
            }
            
            // Market data access (thread-safe read-only)
            FORCE_INLINE PURE_FUNCTION const OrderBook* get_order_book(SymbolId symbol_id) const noexcept {
                auto it = order_books_.find(symbol_id);
                return it != order_books_.end() ? it->second.get() : nullptr;
            }
            
            FORCE_INLINE PURE_FUNCTION Price get_best_bid(SymbolId symbol_id) const noexcept {
                const auto* book = get_order_book(symbol_id);
                if (UNLIKELY(!book)) return 0;
                
                const auto* best_bid = book->get_best_bid();
                return best_bid ? best_bid->price : 0;
            }
            
            FORCE_INLINE PURE_FUNCTION Price get_best_ask(SymbolId symbol_id) const noexcept {
                const auto* book = get_order_book(symbol_id);
                if (UNLIKELY(!book)) return 0;
                
                const auto* best_ask = book->get_best_ask();
                return best_ask ? best_ask->price : 0;
            }
            
            FORCE_INLINE PURE_FUNCTION Price get_mid_price(SymbolId symbol_id) const noexcept {
                const auto* book = get_order_book(symbol_id);
                return book ? book->get_mid_price() : 0;
            }
            
            FORCE_INLINE PURE_FUNCTION Price get_spread(SymbolId symbol_id) const noexcept {
                const auto* book = get_order_book(symbol_id);
                return book ? book->get_spread() : Price(-1);
            }
            
            // Get full market depth (top N levels)
            struct MarketDepth {
                static constexpr size_t MAX_LEVELS = 10;
                
                struct Level {
                    Price price;
                    Quantity quantity;
                    uint32_t order_count;
                };
                
                Level bids[MAX_LEVELS];
                Level asks[MAX_LEVELS];
                size_t bid_count;
                size_t ask_count;
                Timestamp timestamp;
            };
            
            MarketDepth get_market_depth(SymbolId symbol_id, size_t levels = 5) const noexcept;
            
            // Event callbacks (thread-safe registration)
            void set_quote_callback(std::function<void(SymbolId, const PriceLevel*, const PriceLevel*)> callback) noexcept {
                on_best_quote_change_ = std::move(callback);
            }
            
            void set_trade_callback(std::function<void(SymbolId, Price, Quantity)> callback) noexcept {
                on_trade_ = std::move(callback);
            }
            
            void set_statistics_callback(std::function<void(const Statistics&)> callback) noexcept {
                on_statistics_update_ = std::move(callback);
            }
            
            void set_error_callback(std::function<void(ErrorCode, const char*)> callback) noexcept {
                on_error_ = std::move(callback);
            }
            
            // Statistics and monitoring
            Statistics get_statistics() const noexcept;
            void reset_statistics() noexcept;
            
            // Health monitoring
            bool is_healthy() const noexcept;
            Duration get_uptime() const noexcept;
            Duration get_average_processing_latency() const noexcept;
            Duration get_average_end_to_end_latency() const noexcept;
            uint64_t get_message_rate() const noexcept; // Messages per second
            
            // Symbol management
            bool add_symbol(SymbolId symbol_id) noexcept;
            bool remove_symbol(SymbolId symbol_id) noexcept;
            std::vector<SymbolId> get_active_symbols() const noexcept;
            size_t get_symbol_count() const noexcept {
                return active_symbols_.load(std::memory_order_relaxed);
            }
            
            // Advanced features
            bool snapshot_order_book(SymbolId symbol_id, OrderBook& snapshot) const noexcept;
            bool replay_from_snapshot(SymbolId symbol_id, const OrderBook& snapshot) noexcept;
            
            // Component access for advanced usage
            const UDPReceiver* get_network_receiver() const noexcept {
                return network_receiver_.get();
            }
            
            const FPGAInterface* get_fpga_interface() const noexcept {
                return fpga_interface_.get();
            }
            
            // Configuration access
            const Config& get_config() const noexcept {
                return config_;
            }
        };
        
        // Engine builder for fluent configuration
        class MarketDataEngineBuilder {
        private:
            MarketDataEngine::Config config_;
            
        public:
            MarketDataEngineBuilder() = default;
            
            MarketDataEngineBuilder& with_network_config(const UDPReceiver::Config& net_config) {
                config_.network_config = net_config;
                return *this;
            }
            
            MarketDataEngineBuilder& with_fpga(bool enable, const FPGAInterface::FPGAConfig& fpga_config = {}) {
                config_.enable_fpga = enable;
                config_.fpga_config = fpga_config;
                return *this;
            }
            
            MarketDataEngineBuilder& with_cpu_affinity(int processing_cpu, int network_cpu) {
                config_.processing_cpu = processing_cpu;
                config_.network_cpu = network_cpu;
                return *this;
            }
            
            MarketDataEngineBuilder& with_max_symbols(size_t max_symbols) {
                config_.max_symbols = max_symbols;
                return *this;
            }
            
            MarketDataEngineBuilder& with_batch_size(size_t batch_size) {
                config_.batch_size = batch_size;
                return *this;
            }
            
            MarketDataEngineBuilder& with_preloaded_symbols(const std::vector<SymbolId>& symbols) {
                config_.preload_symbols = symbols;
                config_.enable_symbol_preallocation = true;
                return *this;
            }
            
            MarketDataEngineBuilder& enable_latency_measurement(bool enable = true) {
                config_.enable_latency_measurement = enable;
                return *this;
            }
            
            std::unique_ptr<MarketDataEngine> build() {
                if (!config_.is_valid()) {
                    return nullptr;
                }
                return std::make_unique<MarketDataEngine>(config_);
            }
        };
        
        // Factory functions for common configurations
        namespace configs {
            // Minimal configuration for testing
            inline MarketDataEngine::Config minimal() {
                MarketDataEngine::Config config;
                config.enable_fpga = false;
                config.enable_latency_measurement = false;
                config.max_symbols = 100;
                config.message_pool_size = 10000;
                config.order_pool_size = 100000;
                return config;
            }
            
            // Production HFT configuration
            inline MarketDataEngine::Config production_hft() {
                MarketDataEngine::Config config;
                config.enable_fpga = true;
                config.enable_latency_measurement = true;
                config.max_symbols = 10000;
                config.message_pool_size = 1000000;
                config.order_pool_size = 10000000;
                config.batch_size = 64;
                config.enable_symbol_preallocation = true;
                return config;
            }
            
            // Market data recording configuration
            inline MarketDataEngine::Config recording() {
                MarketDataEngine::Config config;
                config.enable_fpga = false;
                config.enable_latency_measurement = true;
                config.max_symbols = 5000;
                config.message_pool_size = 5000000;
                config.order_pool_size = 50000000;
                config.batch_size = 128;
                return config;
            }
        }
        
        // Factory function for backward compatibility
        inline std::unique_ptr<MarketDataEngine> create_hft_engine(const MarketDataEngine::Config& config = {}) {
            auto engine = std::make_unique<MarketDataEngine>(config);
            if (engine->get_state() == MarketDataEngine::EngineState::ERROR) {
                return nullptr;
            }
            return engine;
        }

} // namespace hft
