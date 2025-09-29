#pragma once

#include "common/types.hpp"
#include "network/udp_receiver.hpp"
#include <cstring>
#include <unordered_map>
#include <string>
#include <array>
#include <cmath>

namespace hft {

// Binary message formats for different protocols 
namespace protocol {


// ITCH 5.0 style message format
struct __attribute__((packed)) ITCHHeader {
    uint16_t length;
    uint8_t message_type;
    uint64_t timestamp;

    // Message type constants
    static constexpr uint8_t TYPE_ADD_ORDER = 'A';
    static constexpr uint8_t TYPE_MODIFY_ORDER = 'U';
    static constexpr uint8_t TYPE_DELETE_ORDER = 'D';
    static constexpr uint8_t TYPE_TRADE = 'P';
    static constexpr uint8_t TYPE_QUOTE = 'Q'; 
};

struct __attribute__((packed)) AddOrderMessage {
    ITCHHeader header;
    uint64_t order_id;
    uint8_t side;
    uint32_t shares;
    char symbol[8];
    uint64_t price; // Fixed point, scaled by 10^4

    static constexpr size_t SIZE = sizeof(AddOrderMessage);
};

struct __attribute__((packed)) ModifyOrderMessage {
    ITCHHeader header;
    uint64_t order_id;
    uint32_t new_shares;
    uint32_t padding;

    static constexpr size_t SIZE = sizeof(ModifyOrderMessage);
};

struct __attribute__((packed)) DeleteOrderMessage {
   ITCHHeader header;
   uint64_t order_id;

   static constexpr size_t SIZE = sizeof(DeleteOrderMessage);
};

struct __attribute__((packed)) TradeMessage {
    ITCHHeader header;
    uint64_t order_id;
    uint8_t side;
    uint32_t shares;
    char symbol[8];
    uint64_t price;
    uint64_t match_number;

    static constexpr size_t SIZE = sizeof(TradeMessage);
};

struct __attribute__((packed)) QuoteMessage {
    ITCHHeader header;
    char symbol[8];
    uint64_t bid_price;
    uint32_t bid_quantity;
    uint64_t ask_price;
    uint32_t ask_quantity;

    static constexpr size_t SIZE = sizeof(QuoteMessage); 
};

// FAST (Fix Adapted for Streaming) protocol support 
struct __attribute__((packed)) FASTHeader {
    uint8_t presence_map;
    uint32_t template_id;
    uint64_t sequence_number; 
};

} // namespace protocol

// Parsed message structure
struct alignas(CACHE_LINE_SIZE) ParsedMessage {
    MessageType type;
    SymbolId symbol_id;
    Timestamp receive_timestamp;
    Timestamp exchange_timestamp;
    ErrorCode error_code;
    uint16_t message_length;
    uint16_t padding;

    union {
        struct {
            OrderId order_id;
            Price price;
            Quantity quantity;
            Side side;
        } order;

        struct {
            Price price;
            Quantity quantity;
            uint64_t match_number;
            Side side; 
        } trade;

        struct {
            Price bid_price;
            Quantity bid_quantity;
            Price ask_price;
            Quantity ask_quantity; 
        } quote;
    };

    ParsedMessage() noexcept 
        : type(MessageType::INVALID)
        , symbol_id(0)
        , receive_timestamp{}
        , exchange_timestamp{}
        , error_code(ErrorCode::SUCCESS)
        , message_length(0)
        , padding(0) {}
    
    FORCE_INLINE bool is_valid() const noexcept {
        return type != MessageType::INVALID && 
               symbol_id != 0 && 
               error_code == ErrorCode::SUCCESS;
    }
    
    FORCE_INLINE bool is_order_message() const noexcept {
        return type == MessageType::ORDER_ADD || 
               type == MessageType::ORDER_MODIFY || 
               type == MessageType::ORDER_DELETE;
    }
    
    FORCE_INLINE bool is_trade_message() const noexcept {
        return type == MessageType::TRADE;
    }
};

class MessageParser {
public:
    enum class Protocol : uint8_t {
        ITCH_50,
        FAST,
        CUSTOM_BINARY,
        FIX_BINARY
    };
    
    struct Config {
        Protocol protocol = Protocol::ITCH_50;
        bool validate_checksums = false;
        bool enable_sequence_checking = true;
        bool enable_symbol_caching = true;
        size_t max_message_size = 1500;
        size_t symbol_cache_size = 10000;
        int default_price_scale = 4;  // 10^4 for ITCH
        
        bool is_valid() const noexcept {
            return max_message_size > 0 && 
                   max_message_size <= 65536 &&
                   symbol_cache_size > 0 &&
                   default_price_scale >= 0 && 
                   default_price_scale <= 8;
        }
    };

private:
    Config config_;
    
    // Symbol ID mapping with faster lookup
    std::unordered_map<std::string, SymbolId> symbol_map_;
    std::array<std::string, 65536> reverse_symbol_map_;  // SymbolId -> Symbol name
    SymbolId next_symbol_id_ = 1;
    
    // Sequence number tracking per symbol
    std::unordered_map<SymbolId, uint64_t> expected_sequence_;
    
    // Statistics (separate cache lines)
    CACHE_ALIGNED std::atomic<uint64_t> messages_parsed_{0};
    CACHE_ALIGNED std::atomic<uint64_t> parse_errors_{0};
    CACHE_ALIGNED std::atomic<uint64_t> sequence_errors_{0};
    CACHE_ALIGNED std::atomic<uint64_t> checksum_errors_{0};
    CACHE_ALIGNED std::atomic<uint64_t> symbols_discovered_{0};
    
    // Protocol-specific parsers (implemented inline for performance)
    // Note: Implementations moved inline for better optimization
    
    // Helper functions (implemented inline below for performance)
    // Implementation of protocol-specific parsers
    FORCE_INLINE HOT_PATH bool parse_itch_message_impl(const NetworkMessage* msg, ParsedMessage& parsed) noexcept;
    FORCE_INLINE HOT_PATH bool parse_fast_message_impl(const NetworkMessage* msg, ParsedMessage& parsed) noexcept;
    FORCE_INLINE HOT_PATH bool parse_custom_binary_impl(const NetworkMessage* msg, ParsedMessage& parsed) noexcept;

public:
    explicit MessageParser(const Config& config = {}) : config_(config) {
        if (config_.enable_symbol_caching) {
            symbol_map_.reserve(config_.symbol_cache_size);
            expected_sequence_.reserve(config_.symbol_cache_size);
        }
        
        // Initialize reverse map
        for (auto& name : reverse_symbol_map_) {
            name.clear();
        }
    }
    
    // Non-copyable, movable for efficiency
    MessageParser(const MessageParser&) = delete;
    MessageParser& operator=(const MessageParser&) = delete;
    MessageParser(MessageParser&&) = default;
    MessageParser& operator=(MessageParser&&) = default;
    
    // Main parsing function
    FORCE_INLINE HOT_PATH bool parse_message(const NetworkMessage* network_msg, 
                                             ParsedMessage& parsed_msg) noexcept {
        if (UNLIKELY(!network_msg || network_msg->payload_size == 0)) {
            parse_errors_.fetch_add(1, std::memory_order_relaxed);
            parsed_msg.error_code = ErrorCode::INVALID_MESSAGE;
            return false;
        }
        
        if (UNLIKELY(!validate_message_size(network_msg->payload_size))) {
            parse_errors_.fetch_add(1, std::memory_order_relaxed);
            parsed_msg.error_code = ErrorCode::INVALID_MESSAGE;
            return false;
        }
        
        // Copy timestamp
        parsed_msg.receive_timestamp = network_msg->receive_timestamp;
        parsed_msg.message_length = network_msg->payload_size;
        parsed_msg.error_code = ErrorCode::SUCCESS;
        
        bool success = false;
        switch (config_.protocol) {
            case Protocol::ITCH_50:
                success = parse_itch_message_impl(network_msg, parsed_msg);
                break;
            case Protocol::FAST:
                success = parse_fast_message_impl(network_msg, parsed_msg);
                break;
            case Protocol::CUSTOM_BINARY:
                success = parse_custom_binary_impl(network_msg, parsed_msg);
                break;
            default:
                parsed_msg.error_code = ErrorCode::INVALID_MESSAGE;
                success = false;
        }
        
        if (LIKELY(success)) {
            messages_parsed_.fetch_add(1, std::memory_order_relaxed);
        } else {
            parse_errors_.fetch_add(1, std::memory_order_relaxed);
        }
        
        return success;
    }
    
    // Batch parsing for better performance
    FORCE_INLINE HOT_PATH size_t parse_messages(NetworkMessage** network_msgs, size_t count,
                                                ParsedMessage* parsed_msgs) noexcept {
        size_t parsed_count = 0;
        
        // Prefetch for better cache performance
        for (size_t i = 0; i < count && i < 4; ++i) {
            if (network_msgs[i]) {
                PREFETCH_READ(network_msgs[i]);
            }
        }
        
        for (size_t i = 0; i < count; ++i) {
            // Prefetch ahead
            if (i + 4 < count && network_msgs[i + 4]) {
                PREFETCH_READ(network_msgs[i + 4]);
            }
            
            if (parse_message(network_msgs[i], parsed_msgs[parsed_count])) {
                ++parsed_count;
            }
        }
        
        return parsed_count;
    }
    
    // Symbol management
    SymbolId register_symbol(const std::string& symbol) noexcept {
        auto it = symbol_map_.find(symbol);
        if (it != symbol_map_.end()) {
            return it->second;
        }
        
        SymbolId id = next_symbol_id_++;
        symbol_map_[symbol] = id;
        
        if (id < reverse_symbol_map_.size()) {
            reverse_symbol_map_[id] = symbol;
        }
        
        symbols_discovered_.fetch_add(1, std::memory_order_relaxed);
        return id;
    }
    
    std::string get_symbol_name(SymbolId symbol_id) const noexcept {
        if (symbol_id < reverse_symbol_map_.size() && !reverse_symbol_map_[symbol_id].empty()) {
            return reverse_symbol_map_[symbol_id];
        }
        
        // Fallback: search in map
        for (const auto& [name, id] : symbol_map_) {
            if (id == symbol_id) {
                return name;
            }
        }
        
        return "";
    }
    
    size_t get_symbol_count() const noexcept { 
        return symbol_map_.size(); 
    }
    
    bool has_symbol(const std::string& symbol) const noexcept {
        return symbol_map_.find(symbol) != symbol_map_.end();
    }
    
    SymbolId get_symbol_id(const std::string& symbol) const noexcept {
        auto it = symbol_map_.find(symbol);
        return it != symbol_map_.end() ? it->second : 0;
    }
    
    // Configuration
    void set_protocol(Protocol protocol) noexcept { 
        config_.protocol = protocol; 
    }
    
    Protocol get_protocol() const noexcept { 
        return config_.protocol; 
    }
    
    const Config& get_config() const noexcept {
        return config_;
    }
    
    // Statistics
    struct ParserStats {
        uint64_t messages_parsed;
        uint64_t parse_errors;
        uint64_t sequence_errors;
        uint64_t checksum_errors;
        uint64_t symbols_discovered;
        double error_rate;
        double throughput_msg_per_sec;
    };
    
    ParserStats get_stats() const noexcept {
        uint64_t parsed = messages_parsed_.load(std::memory_order_relaxed);
        uint64_t errors = parse_errors_.load(std::memory_order_relaxed);
        
        return {
            parsed,
            errors,
            sequence_errors_.load(std::memory_order_relaxed),
            checksum_errors_.load(std::memory_order_relaxed),
            symbols_discovered_.load(std::memory_order_relaxed),
            parsed > 0 ? (static_cast<double>(errors) / parsed * 100.0) : 0.0,
            0.0  // Would need timing info for actual throughput
        };
    }
    
    void reset_stats() noexcept {
        messages_parsed_.store(0, std::memory_order_relaxed);
        parse_errors_.store(0, std::memory_order_relaxed);
        sequence_errors_.store(0, std::memory_order_relaxed);
        checksum_errors_.store(0, std::memory_order_relaxed);
        symbols_discovered_.store(0, std::memory_order_relaxed);
    }
    
    // Clear all cached data
    void clear_cache() noexcept {
        symbol_map_.clear();
        expected_sequence_.clear();
        next_symbol_id_ = 1;
        
        for (auto& name : reverse_symbol_map_) {
            name.clear();
        }
    }
    
private:
    // Implementation of protocol-specific parsers
    FORCE_INLINE HOT_PATH bool parse_itch_message_impl(const NetworkMessage* msg, ParsedMessage& parsed) noexcept {
        if (UNLIKELY(msg->payload_size < sizeof(protocol::ITCHHeader))) {
            parsed.error_code = ErrorCode::INVALID_MESSAGE;
            return false;
        }
        
        const auto* header = reinterpret_cast<const protocol::ITCHHeader*>(msg->payload);
        parsed.exchange_timestamp = Timestamp{std::chrono::nanoseconds{header->timestamp}};
        
        switch (header->message_type) {
            case protocol::ITCHHeader::TYPE_ADD_ORDER: {
                if (UNLIKELY(msg->payload_size < sizeof(protocol::AddOrderMessage))) {
                    parsed.error_code = ErrorCode::INVALID_MESSAGE;
                    return false;
                }
                
                const auto* add_msg = reinterpret_cast<const protocol::AddOrderMessage*>(msg->payload);
                parsed.type = MessageType::ORDER_ADD;
                parsed.symbol_id = get_or_create_symbol_id(add_msg->symbol, 8);
                parsed.order.order_id = add_msg->order_id;
                parsed.order.price = decode_price(add_msg->price, config_.default_price_scale);
                parsed.order.quantity = add_msg->shares;
                parsed.order.side = decode_side(add_msg->side);
                break;
            }
            
            case protocol::ITCHHeader::TYPE_MODIFY_ORDER: {
                if (UNLIKELY(msg->payload_size < sizeof(protocol::ModifyOrderMessage))) {
                    parsed.error_code = ErrorCode::INVALID_MESSAGE;
                    return false;
                }
                
                const auto* modify_msg = reinterpret_cast<const protocol::ModifyOrderMessage*>(msg->payload);
                parsed.type = MessageType::ORDER_MODIFY;
                parsed.order.order_id = modify_msg->order_id;
                parsed.order.quantity = modify_msg->new_shares;
                // Note: Symbol and price would need to be looked up from order book
                break;
            }
            
            case protocol::ITCHHeader::TYPE_DELETE_ORDER: {
                if (UNLIKELY(msg->payload_size < sizeof(protocol::DeleteOrderMessage))) {
                    parsed.error_code = ErrorCode::INVALID_MESSAGE;
                    return false;
                }
                
                const auto* delete_msg = reinterpret_cast<const protocol::DeleteOrderMessage*>(msg->payload);
                parsed.type = MessageType::ORDER_DELETE;
                parsed.order.order_id = delete_msg->order_id;
                break;
            }
            
            case protocol::ITCHHeader::TYPE_TRADE: {
                if (UNLIKELY(msg->payload_size < sizeof(protocol::TradeMessage))) {
                    parsed.error_code = ErrorCode::INVALID_MESSAGE;
                    return false;
                }
                
                const auto* trade_msg = reinterpret_cast<const protocol::TradeMessage*>(msg->payload);
                parsed.type = MessageType::TRADE;
                parsed.symbol_id = get_or_create_symbol_id(trade_msg->symbol, 8);
                parsed.trade.price = decode_price(trade_msg->price, config_.default_price_scale);
                parsed.trade.quantity = trade_msg->shares;
                parsed.trade.side = decode_side(trade_msg->side);
                parsed.trade.match_number = trade_msg->match_number;
                break;
            }
            
            case protocol::ITCHHeader::TYPE_QUOTE: {
                if (UNLIKELY(msg->payload_size < sizeof(protocol::QuoteMessage))) {
                    parsed.error_code = ErrorCode::INVALID_MESSAGE;
                    return false;
                }
                
                const auto* quote_msg = reinterpret_cast<const protocol::QuoteMessage*>(msg->payload);
                parsed.type = MessageType::QUOTE;
                parsed.symbol_id = get_or_create_symbol_id(quote_msg->symbol, 8);
                parsed.quote.bid_price = decode_price(quote_msg->bid_price, config_.default_price_scale);
                parsed.quote.bid_quantity = quote_msg->bid_quantity;
                parsed.quote.ask_price = decode_price(quote_msg->ask_price, config_.default_price_scale);
                parsed.quote.ask_quantity = quote_msg->ask_quantity;
                break;
            }
            
            default:
                parsed.error_code = ErrorCode::INVALID_MESSAGE;
                return false;
        }
        
        return true;
    }
    
    FORCE_INLINE HOT_PATH bool parse_fast_message_impl(const NetworkMessage* msg, ParsedMessage& parsed) noexcept {
        if (UNLIKELY(msg->payload_size < sizeof(protocol::FASTHeader))) {
            parsed.error_code = ErrorCode::INVALID_MESSAGE;
            return false;
        }
        
        const auto* header = reinterpret_cast<const protocol::FASTHeader*>(msg->payload);
        const char* data = msg->payload + sizeof(protocol::FASTHeader);
        const char* end = msg->payload + msg->payload_size;
        
        // Validate sequence number if enabled
        if (config_.enable_sequence_checking) {
            if (!validate_sequence_number(0, header->sequence_number)) { // Use 0 for global sequence
                sequence_errors_.fetch_add(1, std::memory_order_relaxed);
                parsed.error_code = ErrorCode::SEQUENCE_GAP;
                return false;
            }
        }
        
        // FAST decoding is template-based, this is a simplified version
        switch (header->template_id) {
            case 1: // Trade template
                parsed.type = MessageType::TRADE;
                parsed.trade.price = read_varint(data, end);
                parsed.trade.quantity = read_varint(data, end);
                break;
                
            case 2: // Quote template  
                parsed.type = MessageType::QUOTE;
                parsed.quote.bid_price = read_varint(data, end);
                parsed.quote.bid_quantity = read_varint(data, end);
                parsed.quote.ask_price = read_varint(data, end);
                parsed.quote.ask_quantity = read_varint(data, end);
                break;
                
            default:
                parsed.error_code = ErrorCode::INVALID_MESSAGE;
                return false;
        }
        
        return data <= end; // Ensure we didn't read past end
    }
    
    FORCE_INLINE HOT_PATH bool parse_custom_binary_impl(const NetworkMessage* msg, ParsedMessage& parsed) noexcept {
        // Simple custom binary format: [type:1][symbol_len:1][symbol:N][data...]
        if (UNLIKELY(msg->payload_size < 2)) {
            parsed.error_code = ErrorCode::INVALID_MESSAGE;
            return false;
        }
        
        const char* data = msg->payload;
        uint8_t msg_type = data[0];
        uint8_t symbol_len = data[1];
        
        if (UNLIKELY(msg->payload_size < 2 + symbol_len + sizeof(uint64_t))) {
            parsed.error_code = ErrorCode::INVALID_MESSAGE;
            return false;
        }
        
        // Extract symbol
        parsed.symbol_id = get_or_create_symbol_id(data + 2, symbol_len);
        const char* payload_data = data + 2 + symbol_len;
        
        switch (msg_type) {
            case 1: // Trade
                parsed.type = MessageType::TRADE;
                parsed.trade.price = *reinterpret_cast<const uint64_t*>(payload_data);
                parsed.trade.quantity = *reinterpret_cast<const uint64_t*>(payload_data + 8);
                break;
                
            case 2: // Quote
                parsed.type = MessageType::QUOTE;
                parsed.quote.bid_price = *reinterpret_cast<const uint64_t*>(payload_data);
                parsed.quote.bid_quantity = *reinterpret_cast<const uint64_t*>(payload_data + 8);
                parsed.quote.ask_price = *reinterpret_cast<const uint64_t*>(payload_data + 16);
                parsed.quote.ask_quantity = *reinterpret_cast<const uint64_t*>(payload_data + 24);
                break;
                
            default:
                parsed.error_code = ErrorCode::INVALID_MESSAGE;
                return false;
        }
        
        return true;
    }
    
    // Helper function implementations
    FORCE_INLINE HOT_PATH SymbolId get_or_create_symbol_id(const char* symbol, size_t length) noexcept {
        // Create null-terminated string for lookup
        std::string symbol_str(symbol, std::min(length, size_t(8))); // Limit to 8 chars
        
        auto it = symbol_map_.find(symbol_str);
        if (it != symbol_map_.end()) {
            return it->second;
        }
        
        // Create new symbol ID
        SymbolId id = next_symbol_id_++;
        symbol_map_[symbol_str] = id;
        
        if (id < reverse_symbol_map_.size()) {
            reverse_symbol_map_[id] = symbol_str;
        }
        
        symbols_discovered_.fetch_add(1, std::memory_order_relaxed);
        return id;
    }
    
    FORCE_INLINE bool validate_sequence_number(SymbolId symbol_id, uint64_t sequence) noexcept {
        if (!config_.enable_sequence_checking) {
            return true;
        }
        
        auto it = expected_sequence_.find(symbol_id);
        if (it == expected_sequence_.end()) {
            expected_sequence_[symbol_id] = sequence + 1;
            return true;
        }
        
        if (sequence == it->second) {
            it->second = sequence + 1;
            return true;
        }
        
        // Sequence gap detected
        it->second = sequence + 1;
        return false;
    }
    
    FORCE_INLINE CONST_FUNCTION Price decode_price(uint64_t raw_price, int scale_factor) const noexcept {
        // Convert from protocol scale to our internal scale (10^8)
        if (scale_factor == 8) {
            return static_cast<Price>(raw_price);
        } else if (scale_factor < 8) {
            return static_cast<Price>(raw_price * (constants::PRICE_SCALE_FACTOR / std::pow(10, scale_factor)));
        } else {
            return static_cast<Price>(raw_price / (std::pow(10, scale_factor) / constants::PRICE_SCALE_FACTOR));
        }
    }
    
    FORCE_INLINE Side decode_side(uint8_t side_byte) const noexcept {
        switch (side_byte) {
            case 'B': case 'b': case 1:
                return Side::BUY;
            case 'S': case 's': case 2:
                return Side::SELL;
            default:
                return Side::INVALID;
        }
    }
    
    FORCE_INLINE uint64_t read_varint(const char*& data, const char* end) const noexcept {
        uint64_t result = 0;
        int shift = 0;
        
        while (data < end && shift < 64) {
            uint8_t byte = *data++;
            result |= static_cast<uint64_t>(byte & 0x7F) << shift;
            
            if ((byte & 0x80) == 0) {
                break;
            }
            
            shift += 7;
        }
        
        return result;
    }
    
    FORCE_INLINE bool validate_message_size(size_t size) const noexcept {
        return size > 0 && size <= config_.max_message_size;
    }
    
    FORCE_INLINE uint64_t hash_symbol(const char* symbol, size_t length) const noexcept {
        // Simple FNV-1a hash
        uint64_t hash = 14695981039346656037ULL;
        for (size_t i = 0; i < length && symbol[i] != '\0'; ++i) {
            hash ^= static_cast<uint64_t>(symbol[i]);
            hash *= 1099511628211ULL;
        }
        return hash;
    }
};

// Specialized zero-copy parser for known message formats
template<typename MessageType>
class ZeroCopyParser {
private:
    static constexpr size_t MESSAGE_SIZE = sizeof(MessageType);
    static constexpr size_t ALIGNMENT = alignof(MessageType);

public:
    FORCE_INLINE HOT_PATH const MessageType* parse(const NetworkMessage* msg) noexcept {
        if (UNLIKELY(!msg || msg->payload_size < MESSAGE_SIZE)) {
            return nullptr;
        }
        
        // Check alignment for optimal access
        const void* ptr = msg->payload;
        if (reinterpret_cast<uintptr_t>(ptr) % ALIGNMENT != 0) {
            // Misaligned - would need to copy or handle carefully
            return nullptr;
        }
        
        // Direct cast - zero copy parsing
        return reinterpret_cast<const MessageType*>(ptr);
    }
    
    FORCE_INLINE HOT_PATH bool validate(const MessageType* msg) noexcept {
        if (!msg) return false;
        
        // Add message-specific validation
        return true;
    }
    
    // Parse with validation
    FORCE_INLINE HOT_PATH const MessageType* parse_validated(const NetworkMessage* msg) noexcept {
        const MessageType* parsed = parse(msg);
        return (parsed && validate(parsed)) ? parsed : nullptr;
    }
    
    static constexpr size_t get_message_size() noexcept {
        return MESSAGE_SIZE;
    }
};

// Template specializations for common message types
using ITCHAddOrderParser = ZeroCopyParser<protocol::AddOrderMessage>;
using ITCHTradeParser = ZeroCopyParser<protocol::TradeMessage>;
using ITCHModifyParser = ZeroCopyParser<protocol::ModifyOrderMessage>;
using ITCHDeleteParser = ZeroCopyParser<protocol::DeleteOrderMessage>;
using ITCHQuoteParser = ZeroCopyParser<protocol::QuoteMessage>;

// Fast batch parser for homogeneous message streams
template<typename MessageType>
class BatchParser {
private:
    ZeroCopyParser<MessageType> parser_;
    
public:
    FORCE_INLINE size_t parse_batch(NetworkMessage** msgs, size_t count, 
                                   const MessageType** parsed) noexcept {
        size_t parsed_count = 0;
        
        for (size_t i = 0; i < count; ++i) {
            parsed[parsed_count] = parser_.parse_validated(msgs[i]);
            if (parsed[parsed_count]) {
                ++parsed_count;
            }
        }
        
        return parsed_count;
    }
};

// Helper function to detect message protocol
inline MessageParser::Protocol detect_protocol(const NetworkMessage* msg) noexcept {
    if (!msg || msg->payload_size < 3) {
        return MessageParser::Protocol::CUSTOM_BINARY;
    }
    
    // Try to detect ITCH by checking message type byte
    uint8_t msg_type = msg->payload[2];
    if (msg_type >= 'A' && msg_type <= 'Z') {
        return MessageParser::Protocol::ITCH_50;
    }
    
    // Check for FAST presence map
    if (msg->payload[0] & 0x80) {
        return MessageParser::Protocol::FAST;
    }
    
    return MessageParser::Protocol::CUSTOM_BINARY;
}

} // namespace hft
