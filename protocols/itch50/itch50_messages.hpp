#pragma once

#include <cstdint>
#include <cstring>

namespace hft {
namespace protocols {
namespace itch50 {

// ITCH 5.0 uses big-endian (network byte order)
// All multi-byte integers must be converted

// Helper functions for byte order conversion
inline uint16_t read_u16_be(const uint8_t *data) {
  return (static_cast<uint16_t>(data[0]) << 8) | static_cast<uint16_t>(data[1]);
}

inline uint32_t read_u32_be(const uint8_t *data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

inline uint64_t read_u64_be(const uint8_t *data) {
  return (static_cast<uint64_t>(data[0]) << 56) |
         (static_cast<uint64_t>(data[1]) << 48) |
         (static_cast<uint64_t>(data[2]) << 40) |
         (static_cast<uint64_t>(data[3]) << 32) |
         (static_cast<uint64_t>(data[4]) << 24) |
         (static_cast<uint64_t>(data[5]) << 16) |
         (static_cast<uint64_t>(data[6]) << 8) | static_cast<uint64_t>(data[7]);
}

// ITCH 5.0 Message Types (sizes include 8-byte timestamp)
enum class MessageType : uint8_t {
  SYSTEM_EVENT = 'S',                // 14 bytes
  STOCK_DIRECTORY = 'R',             // 41 bytes
  STOCK_TRADING_ACTION = 'H',        // 27 bytes
  REG_SHO_RESTRICTION = 'Y',         // 22 bytes
  MARKET_PARTICIPANT_POSITION = 'L', // 28 bytes
  MWCB_DECLINE_LEVEL = 'V',          // 37 bytes
  MWCB_STATUS = 'W',                 // 14 bytes
  IPO_QUOTING_PERIOD = 'K',          // 30 bytes
  ADD_ORDER = 'A',                   // 38 bytes
  ADD_ORDER_MPID = 'F',              // 42 bytes
  ORDER_EXECUTED = 'E',              // 33 bytes
  ORDER_EXECUTED_WITH_PRICE = 'C',   // 38 bytes
  ORDER_CANCEL = 'X',                // 25 bytes
  ORDER_DELETE = 'D',                // 21 bytes
  ORDER_REPLACE = 'U',               // 37 bytes
  TRADE = 'P',                       // 46 bytes
  CROSS_TRADE = 'Q',                 // 42 bytes
  BROKEN_TRADE = 'B',                // 21 bytes
  NOII = 'I',                        // 52 bytes
  RPII = 'N'                         // 22 bytes
};

// System Event Codes
enum class SystemEvent : uint8_t {
  START_OF_MESSAGES = 'O',
  START_OF_SYSTEM_HOURS = 'S',
  START_OF_MARKET_HOURS = 'Q',
  END_OF_MARKET_HOURS = 'M',
  END_OF_SYSTEM_HOURS = 'E',
  END_OF_MESSAGES = 'C',
};

// Buy/Sell Indicator
enum class Side : uint8_t {
  BUY = 'B',
  SELL = 'S',
};

// Stock locate - maps to instrument_id
using StockLocate = uint64_t;

// Message structures (packed, big-endian)
#pragma pack(push, 1)

// Header common to all messages
struct MessageHeader {
  uint16_t stock_locate;    // Always at offset 0
  uint16_t tracking_number; // Always at offset 2
  uint64_t timestamp;       // Always at offset 4 (nanoseconds since midnight)
  uint8_t message_type;     // Always at offset 12
};

// System Event Message (Type 'S')
struct SystemEventMessage {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint8_t message_type; // Always 'S'
  uint8_t event_code;

  static constexpr size_t SIZE = 14;
};

// Stock Directory Message (Type 'R')
struct StockDirectoryMessage {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint8_t message_type; // Always 'R'
  char stock[8];        // Right-padded with spaces
  uint8_t market_category;
  uint8_t financial_status;
  uint32_t round_lot_size;
  uint8_t round_lots_only;
  uint8_t issue_classification;
  char issue_sub_type[2];
  uint8_t authenticity;
  uint8_t ipo_flag;
  uint8_t luld_ref_price_tier;
  uint8_t etp_flag;
  uint32_t etp_leverage_factor;
  uint8_t inverse_indicator;

  static constexpr size_t SIZE = 41;
};

// Add Order Message (Type 'A')
struct AddOrderMessage {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint8_t message_type; // Always 'A'
  uint64_t order_reference_number;
  uint8_t buy_sell_indicator;
  uint32_t shares;
  char stock[8];
  uint32_t price;

  static constexpr size_t SIZE = 38;
};

// Add Order with MPID Message (Type 'F')
struct AddOrderMPIDMessage {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint8_t message_type; // Always 'F'
  uint64_t order_reference_number;
  uint8_t buy_sell_indicator;
  uint32_t shares;
  char stock[8];
  uint32_t price;
  char attribution[4]; // MPID

  static constexpr size_t SIZE = 42;
};

// Order Executed Message (Type 'E')
struct OrderExecutedMessage {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint8_t message_type; // Always 'E'
  uint64_t order_reference_number;
  uint32_t executed_shares;
  uint64_t match_number;

  static constexpr size_t SIZE = 33;
};

// Order Executed with Price Message (Type 'C')
struct OrderExecutedWithPriceMessage {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint8_t message_type; // Always 'C'
  uint64_t order_reference_number;
  uint32_t executed_shares;
  uint64_t match_number;
  uint8_t printable;
  uint32_t execution_price;

  static constexpr size_t SIZE = 38;
};

// Order Cancel Message (Type 'X')
struct OrderCancelMessage {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint8_t message_type; // Always 'X'
  uint64_t order_reference_number;
  uint32_t cancelled_shares;

  static constexpr size_t SIZE = 25;
};

// Order Delete Message (Type 'D')
struct OrderDeleteMessage {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint8_t message_type; // Always 'D'
  uint64_t order_reference_number;

  static constexpr size_t SIZE = 21;
};

// Order Replace Message (Type 'U')
struct OrderReplaceMessage {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint8_t message_type; // Always 'U'
  uint64_t original_order_reference_number;
  uint64_t new_order_reference_number;
  uint32_t shares;
  uint32_t price;

  static constexpr size_t SIZE = 37;
};

// Trade Message (Type 'P')
struct TradeMessage {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint8_t message_type; // Always 'P'
  uint64_t order_reference_number;
  uint8_t buy_sell_indicator;
  uint32_t shares;
  char stock[8];
  uint32_t price;
  uint64_t match_number;

  static constexpr size_t SIZE = 46;
};

// Cross Trade Message (Type 'Q')
struct CrossTradeMessage {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint8_t message_type; // Always 'Q'
  uint64_t shares;
  char stock[8];
  uint32_t cross_price;
  uint64_t match_number;
  uint8_t cross_type;

  static constexpr size_t SIZE = 42;
};

// Broken Trade Message (Type 'B')
struct BrokenTradeMessage {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint8_t message_type; // Always 'B'
  uint64_t match_number;

  static constexpr size_t SIZE = 21;
};

#pragma pack(pop)

// Helper function to get message size by type
inline size_t get_message_size(MessageType type) {
  switch (type) {
  case MessageType::SYSTEM_EVENT:
    return SystemEventMessage::SIZE;
  case MessageType::STOCK_DIRECTORY:
    return StockDirectoryMessage::SIZE;
  case MessageType::STOCK_TRADING_ACTION:
    return 27;
  case MessageType::REG_SHO_RESTRICTION:
    return 22;
  case MessageType::MARKET_PARTICIPANT_POSITION:
    return 28;
  case MessageType::MWCB_DECLINE_LEVEL:
    return 37;
  case MessageType::MWCB_STATUS:
    return 14;
  case MessageType::IPO_QUOTING_PERIOD:
    return 30;
  case MessageType::ADD_ORDER:
    return AddOrderMessage::SIZE;
  case MessageType::ADD_ORDER_MPID:
    return AddOrderMPIDMessage::SIZE;
  case MessageType::ORDER_EXECUTED:
    return OrderExecutedMessage::SIZE;
  case MessageType::ORDER_EXECUTED_WITH_PRICE:
    return OrderExecutedWithPriceMessage::SIZE;
  case MessageType::ORDER_CANCEL:
    return OrderCancelMessage::SIZE;
  case MessageType::ORDER_DELETE:
    return OrderDeleteMessage::SIZE;
  case MessageType::ORDER_REPLACE:
    return OrderReplaceMessage::SIZE;
  case MessageType::TRADE:
    return TradeMessage::SIZE;
  case MessageType::CROSS_TRADE:
    return CrossTradeMessage::SIZE;
  case MessageType::BROKEN_TRADE:
    return BrokenTradeMessage::SIZE;
  case MessageType::NOII:
    return 52;
  case MessageType::RPII:
    return 22;
  default:
    return 0;
  }
}
} // namespace itch50
} // namespace protocols
} // namespace hft