#pragma once

#include "../../core/parser/parser_interface.hpp"
#include "../../core/types.hpp"
#include "itch50_messages.hpp"
#include <string>
#include <unordered_map>

namespace hft {
namespace protocols {
namespace itch50 {

// ITCH 5.0 Parser - converts ITCH messages into NormalizedMessage format
class ItchParser : public core::IParser {
public:
  ItchParser() : messages_parsed_(0), parse_errors_(0) {}

  size_t parse(const core::MessageView &raw_packet,
               core::NormalizedMessage *output,
               size_t max_messages) noexcept override {
    if (!raw_packet.is_valid() || max_messages == 0) {
      return 0;
    }

    size_t messages_parsed = 0;
    const uint8_t *data = raw_packet.data;
    size_t remaining = raw_packet.length;
    size_t offset = 0;

    // ITCH packets can contain multiple messages
    // Each message starts with a 2-byte length field (big-endian)
    while (remaining >= 3 && messages_parsed < max_messages) {
      // Read message length (includes length field iteself)
      uint16_t msg_length = read_u16_be(data + offset);

      if (msg_length < 3 || msg_length > remaining) {
        parse_errors_++;
        break;
      }

      // Message data starts after length field
      const uint8_t *msg_data = data + offset + 2;
      size_t msg_size = msg_length - 2;

      // Parse the message
      if (parse_message(msg_data, msg_size, output[messages_parsed],
                        raw_packet.timestamp)) {
        messages_parsed++;
      }

      offset += msg_length;
      remaining -= msg_length;
    }

    messages_parsed_ += messages_parsed;
    return messages_parsed;
  }

  const char *name() const noexcept override { return "ITCH-5.0"; }

  void get_stats(core::Statistics &stats) const override {
    stats.messages_parsed = messages_parsed_;
    stats.parse_errors = parse_errors_;
  }

  void reset() override {
    messages_parsed_ = 0;
    parse_errors_ = 0;
    stock_map_.clear();
  }

private:
  bool parse_message(const uint8_t *data, size_t length,
                     core::NormalizedMessage &output,
                     core::Timestamp local_timestamp) noexcept {
    if (length < 13) { // Minimum header size
      parse_errors_++;
      return false;
    }

    // Read message type
    MessageType msg_type = static_cast<MessageType>(data[12]);

    // Parse based on message type
    switch (msg_type) {
    case MessageType::SYSTEM_EVENT:
      return parse_system_event(data, length, output, local_timestamp);

    case MessageType::STOCK_DIRECTORY:
      return parse_stock_directory(data, length, output, local_timestamp);

    case MessageType::ADD_ORDER:
      return parse_add_order(data, length, output, local_timestamp);

    case MessageType::ADD_ORDER_MPID:
      return parse_add_order_mpid(data, length, output, local_timestamp);

    case MessageType::ORDER_EXECUTED:
      return parse_order_executed(data, length, output, local_timestamp);

    case MessageType::ORDER_EXECUTED_WITH_PRICE:
      return parse_order_executed_with_price(data, length, output,
                                             local_timestamp);

    case MessageType::ORDER_CANCEL:
      return parse_order_cancel(data, length, output, local_timestamp);

    case MessageType::ORDER_DELETE:
      return parse_order_delete(data, length, output, local_timestamp);

    case MessageType::ORDER_REPLACE:
      return parse_order_replace(data, length, output, local_timestamp);

    case MessageType::TRADE:
      return parse_trade(data, length, output, local_timestamp);

    default:
      // Unsupported message type (not an error, just skip)
      return false;
    }
  }

  bool parse_system_event(const uint8_t *data, size_t length,
                          core::NormalizedMessage &output,
                          core::Timestamp local_timestamp) noexcept {
    if (length < SystemEventMessage::SIZE)
      return false;

    const auto *msg = reinterpret_cast<const SystemEventMessage *>(data);

    output.type = core::NormalizedMessage::Type::SYSTEM_EVENT;
    output.timestamp =
        read_u64_be(reinterpret_cast<const uint8_t *>(&msg->timestamp));
    output.local_timestamp = local_timestamp;
    output.instrument_id = 0; // System events are not instrument-specific
    output.sequence =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->tracking_number));

    return true;
  }
  bool parse_stock_directory(const uint8_t *data, size_t length,
                             core::NormalizedMessage &output,
                             core::Timestamp local_timestamp) noexcept {
    if (length < StockDirectoryMessage::SIZE)
      return false;

    const auto *msg = reinterpret_cast<const StockDirectoryMessage *>(data);

    // Extract stock locate and stock symbol
    uint16_t stock_locate =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->stock_locate));

    // Store mapping for later use
    stock_map_[stock_locate] = std::string(msg->stock, 8);

    // Generate a system event for new stock
    output.type = core::NormalizedMessage::Type::SYSTEM_EVENT;
    output.timestamp =
        read_u64_be(reinterpret_cast<const uint8_t *>(&msg->timestamp));
    output.local_timestamp = local_timestamp;
    output.instrument_id = stock_locate;
    output.sequence =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->tracking_number));

    return true;
  }

  bool parse_add_order(const uint8_t *data, size_t length,
                       core::NormalizedMessage &output,
                       core::Timestamp local_timestamp) noexcept {
    if (length < AddOrderMessage::SIZE)
      return false;

    const auto *msg = reinterpret_cast<const AddOrderMessage *>(data);

    output.type = core::NormalizedMessage::Type::ORDER_ADD;
    output.instrument_id =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->stock_locate));
    output.order_id = read_u64_be(
        reinterpret_cast<const uint8_t *>(&msg->order_reference_number));
    output.timestamp =
        read_u64_be(reinterpret_cast<const uint8_t *>(&msg->timestamp));
    output.local_timestamp = local_timestamp;
    output.side =
        (msg->buy_sell_indicator == static_cast<uint8_t>(Side::BUY)) ? 0 : 1;
    output.quantity =
        read_u32_be(reinterpret_cast<const uint8_t *>(&msg->shares));
    output.price = read_u32_be(reinterpret_cast<const uint8_t *>(&msg->price));
    output.sequence =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->tracking_number));

    return true;
  }

  bool parse_add_order_mpid(const uint8_t *data, size_t length,
                            core::NormalizedMessage &output,
                            core::Timestamp local_timestamp) noexcept {
    if (length < AddOrderMPIDMessage::SIZE)
      return false;

    const auto *msg = reinterpret_cast<const AddOrderMPIDMessage *>(data);

    output.type = core::NormalizedMessage::Type::ORDER_ADD;
    output.instrument_id =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->stock_locate));
    output.order_id = read_u64_be(
        reinterpret_cast<const uint8_t *>(&msg->order_reference_number));
    output.timestamp =
        read_u64_be(reinterpret_cast<const uint8_t *>(&msg->timestamp));
    output.local_timestamp = local_timestamp;
    output.side =
        (msg->buy_sell_indicator == static_cast<uint8_t>(Side::BUY)) ? 0 : 1;
    output.quantity =
        read_u32_be(reinterpret_cast<const uint8_t *>(&msg->shares));
    output.price = read_u32_be(reinterpret_cast<const uint8_t *>(&msg->price));
    output.sequence =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->tracking_number));

    return true;
  }

  bool parse_order_executed(const uint8_t *data, size_t length,
                            core::NormalizedMessage &output,
                            core::Timestamp local_timestamp) noexcept {
    if (length < OrderExecutedMessage::SIZE)
      return false;

    const auto *msg = reinterpret_cast<const OrderExecutedMessage *>(data);

    output.type = core::NormalizedMessage::Type::ORDER_EXECUTE;
    output.instrument_id =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->stock_locate));
    output.order_id = read_u64_be(
        reinterpret_cast<const uint8_t *>(&msg->order_reference_number));
    output.timestamp =
        read_u64_be(reinterpret_cast<const uint8_t *>(&msg->timestamp));
    output.local_timestamp = local_timestamp;
    output.quantity =
        read_u32_be(reinterpret_cast<const uint8_t *>(&msg->executed_shares));
    output.sequence =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->tracking_number));

    return true;
  }

  bool
  parse_order_executed_with_price(const uint8_t *data, size_t length,
                                  core::NormalizedMessage &output,
                                  core::Timestamp local_timestamp) noexcept {
    if (length < OrderExecutedWithPriceMessage::SIZE)
      return false;

    const auto *msg =
        reinterpret_cast<const OrderExecutedWithPriceMessage *>(data);

    output.type = core::NormalizedMessage::Type::ORDER_EXECUTE;
    output.instrument_id =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->stock_locate));
    output.order_id = read_u64_be(
        reinterpret_cast<const uint8_t *>(&msg->order_reference_number));
    output.timestamp =
        read_u64_be(reinterpret_cast<const uint8_t *>(&msg->timestamp));
    output.local_timestamp = local_timestamp;
    output.quantity =
        read_u32_be(reinterpret_cast<const uint8_t *>(&msg->executed_shares));
    output.price =
        read_u32_be(reinterpret_cast<const uint8_t *>(&msg->execution_price));
    output.sequence =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->tracking_number));

    return true;
  }

  bool parse_order_cancel(const uint8_t *data, size_t length,
                          core::NormalizedMessage &output,
                          core::Timestamp local_timestamp) noexcept {
    if (length < OrderCancelMessage::SIZE)
      return false;

    const auto *msg = reinterpret_cast<const OrderCancelMessage *>(data);

    output.type = core::NormalizedMessage::Type::ORDER_MODIFY;
    output.instrument_id =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->stock_locate));
    output.order_id = read_u64_be(
        reinterpret_cast<const uint8_t *>(&msg->order_reference_number));
    output.timestamp =
        read_u64_be(reinterpret_cast<const uint8_t *>(&msg->timestamp));
    output.local_timestamp = local_timestamp;
    output.quantity =
        read_u32_be(reinterpret_cast<const uint8_t *>(&msg->cancelled_shares));
    output.sequence =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->tracking_number));

    return true;
  }

  bool parse_order_delete(const uint8_t *data, size_t length,
                          core::NormalizedMessage &output,
                          core::Timestamp local_timestamp) noexcept {
    if (length < OrderDeleteMessage::SIZE)
      return false;

    const auto *msg = reinterpret_cast<const OrderDeleteMessage *>(data);

    output.type = core::NormalizedMessage::Type::ORDER_DELETE;
    output.instrument_id =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->stock_locate));
    output.order_id = read_u64_be(
        reinterpret_cast<const uint8_t *>(&msg->order_reference_number));
    output.timestamp =
        read_u64_be(reinterpret_cast<const uint8_t *>(&msg->timestamp));
    output.local_timestamp = local_timestamp;
    output.sequence =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->tracking_number));

    return true;
  }

  bool parse_order_replace(const uint8_t *data, size_t length,
                           core::NormalizedMessage &output,
                           core::Timestamp local_timestamp) noexcept {
    if (length < OrderReplaceMessage::SIZE)
      return false;

    const auto *msg = reinterpret_cast<const OrderReplaceMessage *>(data);

    output.type = core::NormalizedMessage::Type::ORDER_MODIFY;
    output.instrument_id =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->stock_locate));
    output.order_id = read_u64_be(
        reinterpret_cast<const uint8_t *>(&msg->new_order_reference_number));
    output.timestamp =
        read_u64_be(reinterpret_cast<const uint8_t *>(&msg->timestamp));
    output.local_timestamp = local_timestamp;
    output.quantity =
        read_u32_be(reinterpret_cast<const uint8_t *>(&msg->shares));
    output.price = read_u32_be(reinterpret_cast<const uint8_t *>(&msg->price));
    output.sequence =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->tracking_number));

    return true;
  }

  bool parse_trade(const uint8_t *data, size_t length,
                   core::NormalizedMessage &output,
                   core::Timestamp local_timestamp) noexcept {
    if (length < TradeMessage::SIZE)
      return false;

    const auto *msg = reinterpret_cast<const TradeMessage *>(data);

    output.type = core::NormalizedMessage::Type::TRADE;
    output.instrument_id =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->stock_locate));
    output.order_id = read_u64_be(
        reinterpret_cast<const uint8_t *>(&msg->order_reference_number));
    output.timestamp =
        read_u64_be(reinterpret_cast<const uint8_t *>(&msg->timestamp));
    output.local_timestamp = local_timestamp;
    output.side =
        (msg->buy_sell_indicator == static_cast<uint8_t>(Side::BUY)) ? 0 : 1;
    output.quantity =
        read_u32_be(reinterpret_cast<const uint8_t *>(&msg->shares));
    output.price = read_u32_be(reinterpret_cast<const uint8_t *>(&msg->price));
    output.sequence =
        read_u16_be(reinterpret_cast<const uint8_t *>(&msg->tracking_number));

    return true;
  }

  // Statistics
  mutable uint64_t messages_parsed_;
  mutable uint64_t parse_errors_;

  // Stock locate -> Symbol mapping
  std::unordered_map<uint16_t, std::string> stock_map_;
};
} // namespace itch50
} // namespace protocols
} // namespace hft