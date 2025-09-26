#include "core/message_parser.hpp"
#include <cstring>
#include <arpa/inet.h>

namespace hft {

bool MessageParser::parse_itch_message(const NetworkMessage* msg, ParsedMessage& parsed) noexcept {
    if (msg->payload_size < sizeof(protocol::ITCHHeader)) {
        return false;
    }
    
    const auto* header = reinterpret_cast<const protocol::ITCHHeader*>(msg->payload);
    
    // Convert from network byte order
    uint16_t length = ntohs(header->length);
    uint64_t timestamp = be64toh(header->timestamp);
    
    if (length != msg->payload_size) {
        return false; // Length mismatch
    }
    
    parsed.exchange_timestamp = Timestamp(Duration(timestamp));
    
    switch (header->message_type) {
        case 'A': { // Add Order
            if (msg->payload_size < sizeof(protocol::AddOrderMessage)) {
                return false;
            }
            
            const auto* add_msg = reinterpret_cast<const protocol::AddOrderMessage*>(msg->payload);
            
            parsed.type = MessageType::ORDER_ADD;
            parsed.order.order_id = be64toh(add_msg->order_id);
            parsed.order.side = add_msg->side == 'B' ? Side::BUY : Side::SELL;
            parsed.order.quantity = ntohl(add_msg->shares);
            parsed.order.price = decode_price(be64toh(add_msg->price));
            
            // Extract symbol
            char symbol_str[9] = {0};
            std::memcpy(symbol_str, add_msg->symbol, 8);
            parsed.symbol_id = get_or_create_symbol_id(symbol_str, 8);
            
            break;
        }
        
        case 'U': { // Modify Order
            if (msg->payload_size < sizeof(protocol::ModifyOrderMessage)) {
                return false;
            }
            
            const auto* mod_msg = reinterpret_cast<const protocol::ModifyOrderMessage*>(msg->payload);
            
            parsed.type = MessageType::ORDER_MODIFY;
            parsed.order.order_id = be64toh(mod_msg->order_id);
            parsed.order.quantity = ntohl(mod_msg->new_shares);
            
            break;
        }
        
        case 'D': { // Delete Order
            if (msg->payload_size < sizeof(protocol::DeleteOrderMessage)) {
                return false;
            }
            
            const auto* del_msg = reinterpret_cast<const protocol::DeleteOrderMessage*>(msg->payload);
            
            parsed.type = MessageType::ORDER_DELETE;
            parsed.order.order_id = be64toh(del_msg->order_id);
            
            break;
        }
        
        case 'P': { // Trade
            if (msg->payload_size < sizeof(protocol::TradeMessage)) {
                return false;
            }
            
            const auto* trade_msg = reinterpret_cast<const protocol::TradeMessage*>(msg->payload);
            
            parsed.type = MessageType::TRADE;
            parsed.trade.price = decode_price(be64toh(trade_msg->price));
            parsed.trade.quantity = ntohl(trade_msg->shares);
            parsed.trade.match_number = be64toh(trade_msg->match_number);
            
            // Extract symbol
            char symbol_str[9] = {0};
            std::memcpy(symbol_str, trade_msg->symbol, 8);
            parsed.symbol_id = get_or_create_symbol_id(symbol_str, 8);
            
            break;
        }
        
        default:
            return false; // Unknown message type
    }
    
    return true;
}

bool MessageParser::parse_fast_message(const NetworkMessage* msg, ParsedMessage& parsed) noexcept {
    if (msg->payload_size < sizeof(protocol::FASTHeader)) {
        return false;
    }
    
    const char* data = msg->payload;
    const char* end = data + msg->payload_size;
    
    // Parse FAST header
    const auto* header = reinterpret_cast<const protocol::FASTHeader*>(data);
    data += sizeof(protocol::FASTHeader);
    
    uint32_t template_id = ntohl(header->template_id);
    uint64_t sequence = be64toh(header->sequence_number);
    
    // Validate sequence number if enabled
    if (config_.enable_sequence_checking) {
        if (!validate_sequence_number(parsed.symbol_id, sequence)) {
            return false;
        }
    }
    
    // Parse fields based on template ID
    switch (template_id) {
        case 1: { // Quote template
            parsed.type = MessageType::QUOTE;
            
            // Parse bid price (varint)
            uint64_t bid_price = read_varint(data, end);
            parsed.quote.bid_price = decode_price(bid_price);
            
            // Parse bid quantity (varint)
            uint64_t bid_qty = read_varint(data, end);
            parsed.quote.bid_quantity = bid_qty;
            
            // Parse ask price (varint)
            uint64_t ask_price = read_varint(data, end);
            parsed.quote.ask_price = decode_price(ask_price);
            
            // Parse ask quantity (varint)
            uint64_t ask_qty = read_varint(data, end);
            parsed.quote.ask_quantity = ask_qty;
            
            break;
        }
        
        case 2: { // Trade template
            parsed.type = MessageType::TRADE;
            
            uint64_t price = read_varint(data, end);
            parsed.trade.price = decode_price(price);
            
            uint64_t quantity = read_varint(data, end);
            parsed.trade.quantity = quantity;
            
            break;
        }
        
        default:
            return false; // Unknown template
    }
    
    return data <= end; // Ensure we didn't read past the end
}

bool MessageParser::parse_custom_binary(const NetworkMessage* msg, ParsedMessage& parsed) noexcept {
    // Simple custom binary format for demonstration
    if (msg->payload_size < 16) { // Minimum header size
        return false;
    }
    
    const char* data = msg->payload;
    
    // Read header
    uint32_t msg_type = *reinterpret_cast<const uint32_t*>(data);
    data += 4;
    
    uint32_t symbol_id = *reinterpret_cast<const uint32_t*>(data);
    data += 4;
    
    uint64_t timestamp = *reinterpret_cast<const uint64_t*>(data);
    data += 8;
    
    parsed.symbol_id = symbol_id;
    parsed.exchange_timestamp = Timestamp(Duration(timestamp));
    
    switch (msg_type) {
        case 1: { // Order
            if (msg->payload_size < 40) return false;
            
            parsed.type = MessageType::ORDER_ADD;
            parsed.order.order_id = *reinterpret_cast<const uint64_t*>(data);
            data += 8;
            
            parsed.order.price = *reinterpret_cast<const int64_t*>(data);
            data += 8;
            
            parsed.order.quantity = *reinterpret_cast<const uint64_t*>(data);
            data += 8;
            
            parsed.order.side = *reinterpret_cast<const uint8_t*>(data) == 1 ? Side::BUY : Side::SELL;
            
            break;
        }
        
        case 2: { // Trade
            if (msg->payload_size < 32) return false;
            
            parsed.type = MessageType::TRADE;
            parsed.trade.price = *reinterpret_cast<const int64_t*>(data);
            data += 8;
            
            parsed.trade.quantity = *reinterpret_cast<const uint64_t*>(data);
            data += 8;
            
            break;
        }
        
        default:
            return false;
    }
    
    return true;
}

SymbolId MessageParser::get_or_create_symbol_id(const char* symbol, size_t length) noexcept {
    // Remove trailing spaces/nulls
    while (length > 0 && (symbol[length-1] == ' ' || symbol[length-1] == '\0')) {
        --length;
    }
    
    std::string symbol_str(symbol, length);
    
    auto it = symbol_map_.find(symbol_str);
    if (it != symbol_map_.end()) {
        return it->second;
    }
    
    // Create new symbol ID
    SymbolId new_id = next_symbol_id_++;
    symbol_map_[symbol_str] = new_id;
    
    return new_id;
}

bool MessageParser::validate_sequence_number(SymbolId symbol_id, uint64_t sequence) noexcept {
    auto it = expected_sequence_.find(symbol_id);
    if (it == expected_sequence_.end()) {
        expected_sequence_[symbol_id] = sequence + 1;
        return true;
    }
    
    if (sequence == it->second) {
        ++it->second;
        return true;
    }
    
    // Sequence gap detected
    it->second = sequence + 1;
    return false;
}

Price MessageParser::decode_price(uint64_t raw_price, int scale_factor) const noexcept {
    // Convert from scaled integer to our internal price representation
    return static_cast<Price>(raw_price);
}

uint64_t MessageParser::read_varint(const char*& data, const char* end) const noexcept {
    uint64_t result = 0;
    int shift = 0;
    
    while (data < end && shift < 64) {
        uint8_t byte = static_cast<uint8_t>(*data++);
        
        if (byte & 0x80) {
            result |= static_cast<uint64_t>(byte & 0x7F) << shift;
            shift += 7;
        } else {
            result |= static_cast<uint64_t>(byte) << shift;
            break;
        }
    }
    
    return result;
}

SymbolId MessageParser::register_symbol(const std::string& symbol) noexcept {
    auto it = symbol_map_.find(symbol);
    if (it != symbol_map_.end()) {
        return it->second;
    }
    
    SymbolId new_id = next_symbol_id_++;
    symbol_map_[symbol] = new_id;
    return new_id;
}

std::string MessageParser::get_symbol_name(SymbolId symbol_id) const noexcept {
    for (const auto& pair : symbol_map_) {
        if (pair.second == symbol_id) {
            return pair.first;
        }
    }
    return "";
}

MessageParser::ParserStats MessageParser::get_stats() const noexcept {
    // In a real implementation, these would be tracked
    return {0, 0, 0, 0, symbol_map_.size()};
}

void MessageParser::reset_stats() noexcept {
    // Reset statistics counters
}

} // namespace hft
