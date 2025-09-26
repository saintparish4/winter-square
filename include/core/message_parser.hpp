#pragma once

#include "common/types.hpp"
#include "network/udp_receiver.hpp"
#include <cstring>

namespace hft {

// Binary message formats for different protocols
namespace protocol {

// ITCH 5.0 style message format
struct __attribute__((packed)) ITCHHeader {
    uint16_t length;
    uint8_t message_type;
    uint64_t timestamp;
};

struct __attribute__((packed)) AddOrderMessage {
    ITCHHeader header;
    uint64_t order_id;
    uint8_t side;
    uint32_t shares;
    char symbol[8];
    uint64_t price; // Fixed point, scaled by 10^4
};

struct __attribute__((packed)) ModifyOrderMessage {
    ITCHHeader header;
    uint64_t order_id;
    uint32_t new_shares;
};

struct __attribute__((packed)) DeleteOrderMessage {
    ITCHHeader header;
    uint64_t order_id;
};

struct __attribute__((packed)) TradeMessage {
    ITCHHeader header;
    uint64_t order_id;
    uint8_t side;
    uint32_t shares;
    char symbol[8];
    uint64_t price;
    uint64_t match_number;
};

// FAST (FIX Adapted for STreaming) protocol support
struct __attribute__((packed)) FASTHeader {
    uint8_t presence_map;
    uint32_t template_id;
    uint64_t sequence_number;
};

} // namespace protocol

// Parsed message structure
struct ParsedMessage {
    MessageType type;
    SymbolId symbol_id;
    Timestamp receive_timestamp;
    Timestamp exchange_timestamp;
    
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
        } trade;
        
        struct {
            Price bid_price;
            Quantity bid_quantity;
            Price ask_price;
            Quantity ask_quantity;
        } quote;
    };
    
    ParsedMessage() : type(MessageType::HEARTBEAT), symbol_id(0) {}
};

class MessageParser {
public:
    enum class Protocol {
        ITCH_50,
        FAST,
        CUSTOM_BINARY,
        FIX_BINARY
    };
    
    struct Config {
        Protocol protocol = Protocol::ITCH_50;
        bool validate_checksums = false;
        bool enable_sequence_checking = true;
        size_t max_message_size = 1500;
    };

private:
    Config config_;
    
    // Symbol ID mapping (symbol string -> ID)
    std::unordered_map<std::string, SymbolId> symbol_map_;
    SymbolId next_symbol_id_ = 1;
    
    // Sequence number tracking
    std::unordered_map<SymbolId, uint64_t> expected_sequence_;
    
    // Protocol-specific parsers
    FORCE_INLINE bool parse_itch_message(const NetworkMessage* msg, ParsedMessage& parsed) noexcept;
    FORCE_INLINE bool parse_fast_message(const NetworkMessage* msg, ParsedMessage& parsed) noexcept;
    FORCE_INLINE bool parse_custom_binary(const NetworkMessage* msg, ParsedMessage& parsed) noexcept;
    
    // Helper functions
    FORCE_INLINE SymbolId get_or_create_symbol_id(const char* symbol, size_t length) noexcept;
    FORCE_INLINE bool validate_sequence_number(SymbolId symbol_id, uint64_t sequence) noexcept;
    FORCE_INLINE Price decode_price(uint64_t raw_price, int scale_factor = 4) const noexcept;
    FORCE_INLINE uint64_t read_varint(const char*& data, const char* end) const noexcept;

public:
    explicit MessageParser(const Config& config = {}) : config_(config) {
        symbol_map_.reserve(10000);
        expected_sequence_.reserve(10000);
    }
    
    // Main parsing function
    FORCE_INLINE bool parse_message(const NetworkMessage* network_msg, ParsedMessage& parsed_msg) noexcept {
        if (UNLIKELY(!network_msg || network_msg->payload_size == 0)) {
            return false;
        }
        
        parsed_msg.receive_timestamp = network_msg->receive_timestamp;
        
        switch (config_.protocol) {
            case Protocol::ITCH_50:
                return parse_itch_message(network_msg, parsed_msg);
            case Protocol::FAST:
                return parse_fast_message(network_msg, parsed_msg);
            case Protocol::CUSTOM_BINARY:
                return parse_custom_binary(network_msg, parsed_msg);
            default:
                return false;
        }
    }
    
    // Batch parsing for better performance
    FORCE_INLINE size_t parse_messages(NetworkMessage** network_msgs, size_t count,
                                      ParsedMessage* parsed_msgs) noexcept {
        size_t parsed_count = 0;
        
        for (size_t i = 0; i < count; ++i) {
            if (parse_message(network_msgs[i], parsed_msgs[parsed_count])) {
                ++parsed_count;
            }
        }
        
        return parsed_count;
    }
    
    // Symbol management
    SymbolId register_symbol(const std::string& symbol) noexcept;
    std::string get_symbol_name(SymbolId symbol_id) const noexcept;
    size_t get_symbol_count() const noexcept { return symbol_map_.size(); }
    
    // Configuration
    void set_protocol(Protocol protocol) noexcept { config_.protocol = protocol; }
    Protocol get_protocol() const noexcept { return config_.protocol; }
    
    // Statistics
    struct ParserStats {
        uint64_t messages_parsed;
        uint64_t parse_errors;
        uint64_t sequence_errors;
        uint64_t checksum_errors;
        uint64_t symbols_discovered;
    };
    
    ParserStats get_stats() const noexcept;
    void reset_stats() noexcept;
};

// Specialized zero-copy parser for known message formats
template<typename MessageType>
class ZeroCopyParser {
private:
    static constexpr size_t MESSAGE_SIZE = sizeof(MessageType);

public:
    FORCE_INLINE const MessageType* parse(const NetworkMessage* msg) noexcept {
        if (UNLIKELY(!msg || msg->payload_size < MESSAGE_SIZE)) {
            return nullptr;
        }
        
        // Direct cast - zero copy parsing
        return reinterpret_cast<const MessageType*>(msg->payload);
    }
    
    FORCE_INLINE bool validate(const MessageType* msg) noexcept {
        // Add validation logic here
        return msg != nullptr;
    }
};

// Template specializations for common message types
using ITCHAddOrderParser = ZeroCopyParser<protocol::AddOrderMessage>;
using ITCHTradeParser = ZeroCopyParser<protocol::TradeMessage>;
using ITCHModifyParser = ZeroCopyParser<protocol::ModifyOrderMessage>;
using ITCHDeleteParser = ZeroCopyParser<protocol::DeleteOrderMessage>;

} // namespace hft
