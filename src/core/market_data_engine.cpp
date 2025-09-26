#include "core/market_data_engine.hpp"
#include <algorithm>

namespace hft {

MarketDataEngine::MarketDataEngine(const Config& config) 
    : config_(config) {
    
    // Initialize memory pools
    message_pool_ = std::make_unique<ObjectPool<NetworkMessage>>(config_.message_pool_size);
    order_pool_ = std::make_unique<ObjectPool<Order>>(config_.order_pool_size);
    
    // Initialize network receiver
    network_receiver_ = std::make_unique<UDPReceiver>(config_.network_config);
    
    // Initialize FPGA interface if enabled
    if (config_.enable_fpga) {
        fpga_interface_ = std::make_unique<FPGAInterface>(config_.fpga_config);
    }
    
    // Initialize message parser
    message_parser_ = std::make_unique<MessageParser>();
    
    // Reserve space for order books
    order_books_.reserve(config_.max_symbols);
}

MarketDataEngine::~MarketDataEngine() {
    shutdown();
}

bool MarketDataEngine::initialize() noexcept {
    // Initialize FPGA if enabled
    if (fpga_interface_ && !fpga_interface_->initialize()) {
        // FPGA initialization failed, continue without it
        fpga_interface_.reset();
    }
    
    return true;
}

bool MarketDataEngine::start() noexcept {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    
    // Start network receiver
    if (!network_receiver_->start()) {
        return false;
    }
    
    // Start processing thread
    running_.store(true, std::memory_order_release);
    processing_thread_ = std::thread(&MarketDataEngine::processing_loop, this);
    
    return true;
}

void MarketDataEngine::stop() noexcept {
    running_.store(false, std::memory_order_release);
    
    // Stop network receiver
    network_receiver_->stop();
    
    // Wait for processing thread
    if (processing_thread_.joinable()) {
        processing_thread_.join();
    }
}

void MarketDataEngine::shutdown() noexcept {
    stop();
    
    // Cleanup FPGA
    if (fpga_interface_) {
        fpga_interface_->cleanup();
    }
    
    // Clear order books
    for (auto& pair : order_books_) {
        pair.second->clear();
    }
    order_books_.clear();
}

void MarketDataEngine::processing_loop() noexcept {
    // Configure thread for high performance
    configure_hft_thread(config_.processing_cpu);
    
    while (running_.load(std::memory_order_acquire)) {
        // Process received messages
        NetworkMessage* msg = network_receiver_->try_get_message();
        if (msg) {
            if (process_message(msg)) {
                messages_processed_.fetch_add(1, std::memory_order_relaxed);
            } else {
                parse_errors_.fetch_add(1, std::memory_order_relaxed);
            }
            
            network_receiver_->return_message(msg);
        }
        
        // Process FPGA messages if available
        if (fpga_interface_) {
            auto* hw_msg = fpga_interface_->receive_from_fpga();
            if (hw_msg) {
                // Process hardware-accelerated message
                messages_processed_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        
        // Brief pause to prevent 100% CPU usage
        CPU_PAUSE();
    }
}

bool MarketDataEngine::process_message(NetworkMessage* msg) noexcept {
    LatencyMeasurement latency_measure(&messages_processed_, nullptr);
    
    // Parse the message
    ParsedMessage parsed;
    if (!message_parser_->parse_message(msg, parsed)) {
        return false;
    }
    
    // Record end-to-end latency
    auto now = HighPrecisionTimer::get_timestamp();
    auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - msg->receive_timestamp).count();
    end_to_end_latency_stats_.record_latency(latency_ns);
    
    // Update order book if enabled
    if (config_.enable_order_book_processing) {
        if (update_order_book(parsed)) {
            order_book_updates_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    
    // Send to FPGA for hardware acceleration
    if (fpga_interface_) {
        FPGAInterface::HWMessage hw_msg;
        hw_msg.type = parsed.type;
        hw_msg.symbol_id = parsed.symbol_id;
        hw_msg.hw_timestamp = parsed.exchange_timestamp;
        
        switch (parsed.type) {
            case MessageType::ORDER_ADD:
            case MessageType::ORDER_MODIFY:
            case MessageType::ORDER_DELETE:
                hw_msg.order_data.order_id = parsed.order.order_id;
                hw_msg.order_data.price = parsed.order.price;
                hw_msg.order_data.quantity = parsed.order.quantity;
                hw_msg.order_data.side = parsed.order.side;
                break;
                
            case MessageType::TRADE:
                hw_msg.trade_data.price = parsed.trade.price;
                hw_msg.trade_data.quantity = parsed.trade.quantity;
                break;
                
            default:
                break;
        }
        
        fpga_interface_->send_message_to_fpga(hw_msg);
    }
    
    return true;
}

bool MarketDataEngine::update_order_book(const ParsedMessage& parsed_msg) noexcept {
    OrderBook* book = get_or_create_order_book(parsed_msg.symbol_id);
    if (!book) {
        return false;
    }
    
    // Store previous best bid/ask for change detection
    const PriceLevel* prev_best_bid = book->get_best_bid();
    const PriceLevel* prev_best_ask = book->get_best_ask();
    
    Price prev_bid_price = prev_best_bid ? prev_best_bid->price : 0;
    Price prev_ask_price = prev_best_ask ? prev_best_ask->price : 0;
    
    bool updated = false;
    
    switch (parsed_msg.type) {
        case MessageType::ORDER_ADD:
            updated = book->add_order(parsed_msg.order.order_id,
                                    parsed_msg.order.price,
                                    parsed_msg.order.quantity,
                                    parsed_msg.order.side);
            break;
            
        case MessageType::ORDER_MODIFY:
            updated = book->modify_order(parsed_msg.order.order_id,
                                       parsed_msg.order.quantity);
            break;
            
        case MessageType::ORDER_DELETE:
            updated = book->cancel_order(parsed_msg.order.order_id);
            break;
            
        case MessageType::TRADE:
            // Trades don't directly update order book, but we can trigger callbacks
            if (on_trade_) {
                on_trade_(parsed_msg.symbol_id, parsed_msg.trade.price, 
                         parsed_msg.trade.quantity);
            }
            return true;
            
        default:
            return false;
    }
    
    if (updated) {
        // Check if best bid/ask changed
        const PriceLevel* new_best_bid = book->get_best_bid();
        const PriceLevel* new_best_ask = book->get_best_ask();
        
        Price new_bid_price = new_best_bid ? new_best_bid->price : 0;
        Price new_ask_price = new_best_ask ? new_best_ask->price : 0;
        
        if (new_bid_price != prev_bid_price || new_ask_price != prev_ask_price) {
            if (on_best_quote_change_) {
                on_best_quote_change_(parsed_msg.symbol_id, new_best_bid, new_best_ask);
            }
        }
    }
    
    return updated;
}

OrderBook* MarketDataEngine::get_or_create_order_book(SymbolId symbol_id) noexcept {
    auto it = order_books_.find(symbol_id);
    if (it != order_books_.end()) {
        return it->second.get();
    }
    
    // Create new order book
    auto book = std::make_unique<OrderBook>(order_pool_.get());
    OrderBook* book_ptr = book.get();
    order_books_[symbol_id] = std::move(book);
    
    return book_ptr;
}

bool MarketDataEngine::add_symbol(SymbolId symbol_id) noexcept {
    if (order_books_.find(symbol_id) != order_books_.end()) {
        return true; // Already exists
    }
    
    if (order_books_.size() >= config_.max_symbols) {
        return false; // Too many symbols
    }
    
    get_or_create_order_book(symbol_id);
    return true;
}

bool MarketDataEngine::remove_symbol(SymbolId symbol_id) noexcept {
    auto it = order_books_.find(symbol_id);
    if (it == order_books_.end()) {
        return false;
    }
    
    it->second->clear();
    order_books_.erase(it);
    return true;
}

std::vector<SymbolId> MarketDataEngine::get_active_symbols() const noexcept {
    std::vector<SymbolId> symbols;
    symbols.reserve(order_books_.size());
    
    for (const auto& pair : order_books_) {
        symbols.push_back(pair.first);
    }
    
    return symbols;
}

MarketDataEngine::Statistics MarketDataEngine::get_statistics() const noexcept {
    Statistics stats;
    stats.messages_received = network_receiver_->get_messages_received();
    stats.messages_processed = messages_processed_.load(std::memory_order_acquire);
    stats.parse_errors = parse_errors_.load(std::memory_order_acquire);
    stats.order_book_updates = order_book_updates_.load(std::memory_order_acquire);
    stats.symbols_active = order_books_.size();
    stats.processing_latency = processing_latency_stats_.get_stats();
    stats.end_to_end_latency = end_to_end_latency_stats_.get_stats();
    
    return stats;
}

void MarketDataEngine::reset_statistics() noexcept {
    processing_latency_stats_.reset();
    end_to_end_latency_stats_.reset();
    messages_processed_.store(0, std::memory_order_release);
    parse_errors_.store(0, std::memory_order_release);
    order_book_updates_.store(0, std::memory_order_release);
}

bool MarketDataEngine::is_healthy() const noexcept {
    // Check if network receiver is running
    if (!network_receiver_->is_running()) {
        return false;
    }
    
    // Check FPGA health if enabled
    if (fpga_interface_ && !fpga_interface_->is_healthy()) {
        return false;
    }
    
    // Check processing thread
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    
    return true;
}

Duration MarketDataEngine::get_average_processing_latency() const noexcept {
    auto stats = processing_latency_stats_.get_stats();
    return Duration(stats.avg_latency_ns);
}

Duration MarketDataEngine::get_average_end_to_end_latency() const noexcept {
    auto stats = end_to_end_latency_stats_.get_stats();
    return Duration(stats.avg_latency_ns);
}

// Factory function
std::unique_ptr<MarketDataEngine> create_hft_engine(const MarketDataEngine::Config& config) {
    return std::make_unique<MarketDataEngine>(config);
}

} // namespace hft
