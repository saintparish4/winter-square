#include "../core/types.hpp"
#include "../protocols/itch50/itch50_messages.hpp"
#include "../protocols/itch50/itch50_parser.hpp"
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>

using namespace hft::protocols::itch50;
using namespace hft::core;

// Helper to create ITCH messages
class ItchMessageBuilder {
public:
  // Write big-endian values
  static void write_u16_be(uint8_t *dest, uint16_t value) {
    dest[0] = (value >> 8) & 0xFF;
    dest[1] = value & 0xFF;
  }

  static void write_u32_be(uint8_t *dest, uint32_t value) {
    dest[0] = (value >> 24) & 0xFF;
    dest[1] = (value >> 16) & 0xFF;
    dest[2] = (value >> 8) & 0xFF;
    dest[3] = value & 0xFF;
  }

  static void write_u64_be(uint8_t *dest, uint64_t value) {
    dest[0] = (value >> 56) & 0xFF;
    dest[1] = (value >> 48) & 0xFF;
    dest[2] = (value >> 40) & 0xFF;
    dest[3] = (value >> 32) & 0xFF;
    dest[4] = (value >> 24) & 0xFF;
    dest[5] = (value >> 16) & 0xFF;
    dest[6] = (value >> 8) & 0xFF;
    dest[7] = value & 0xFF;
  }

  // Build System Event message
  static std::vector<uint8_t> build_system_event(uint16_t stock_locate,
                                                 uint16_t tracking,
                                                 uint64_t timestamp,
                                                 char event_code) {
    std::vector<uint8_t> msg(SystemEventMessage::SIZE);
    write_u16_be(&msg[0], stock_locate);
    write_u16_be(&msg[2], tracking);
    write_u64_be(&msg[4], timestamp);
    msg[12] = 'S';
    msg[13] = event_code;
    return msg;
  }

  // Build Add Order message
  static std::vector<uint8_t>
  build_add_order(uint16_t stock_locate, uint16_t tracking, uint64_t timestamp,
                  uint64_t order_ref, char side, uint32_t shares,
                  const char *stock, uint32_t price) {
    std::vector<uint8_t> msg(AddOrderMPIDMessage::SIZE);
    write_u16_be(&msg[0], stock_locate);
    write_u16_be(&msg[2], tracking);
    write_u64_be(&msg[4], timestamp);
    msg[12] = 'A';
    write_u64_be(&msg[13], order_ref);
    msg[21] = side;
    write_u32_be(&msg[22], shares);
    std::memcpy(&msg[26], stock, 8);
    write_u32_be(&msg[34], price);
    return msg;
  }

  // Build Order Executed message
  static std::vector<uint8_t>
  build_order_executed(uint16_t stock_locate, uint16_t tracking,
                       uint64_t timestamp, uint64_t order_ref,
                       uint32_t executed_shares, uint64_t match_number) {
    std::vector<uint8_t> msg(OrderExecutedMessage::SIZE);
    write_u16_be(&msg[0], stock_locate);
    write_u16_be(&msg[2], tracking);
    write_u64_be(&msg[4], timestamp);
    msg[12] = 'E';
    write_u64_be(&msg[13], order_ref);
    write_u32_be(&msg[21], executed_shares);
    write_u64_be(&msg[25], match_number);
    return msg;
  }

  // Build Order Delete message
  static std::vector<uint8_t> build_order_delete(uint16_t stock_locate,
                                                 uint16_t tracking,
                                                 uint64_t timestamp,
                                                 uint64_t order_ref) {
    std::vector<uint8_t> msg(OrderDeleteMessage::SIZE);
    write_u16_be(&msg[0], stock_locate);
    write_u16_be(&msg[2], tracking);
    write_u64_be(&msg[4], timestamp);
    msg[12] = 'D';
    write_u64_be(&msg[13], order_ref);
    return msg;
  }

  // Build Trade message
  static std::vector<uint8_t>
  build_trade(uint16_t stock_locate, uint16_t tracking, uint64_t timestamp,
              uint64_t order_ref, char side, uint32_t shares, const char *stock,
              uint32_t price, uint64_t match_number) {
    std::vector<uint8_t> msg(TradeMessage::SIZE);
    write_u16_be(&msg[0], stock_locate);
    write_u16_be(&msg[2], tracking);
    write_u64_be(&msg[4], timestamp);
    msg[12] = 'P';
    write_u64_be(&msg[13], order_ref);
    msg[21] = side;
    write_u32_be(&msg[22], shares);
    std::memcpy(&msg[26], stock, 8);
    write_u32_be(&msg[34], price);
    write_u64_be(&msg[38], match_number);
    return msg;
  }

  // Build ITCH packet with message length headers
  static std::vector<uint8_t>
  build_packet(const std::vector<std::vector<uint8_t>> &messages) {
    std::vector<uint8_t> packet;

    for (const auto &msg : messages) {
      // Add 2-byte length header (length + 2 for the header itself)
      uint16_t length = static_cast<uint16_t>(msg.size() + 2);
      uint8_t len_bytes[2];
      write_u16_be(len_bytes, length);
      packet.push_back(len_bytes[0]);
      packet.push_back(len_bytes[1]);
      packet.insert(packet.end(), msg.begin(), msg.end());
    }

    return packet;
  }
};

// Test 1: Parse System Event
void test_system_event() {
  ItchParser parser;

  auto msg =
      ItchMessageBuilder::build_system_event(1, 100, 12345678900000ULL, 'O');
  auto packet = ItchMessageBuilder::build_packet({msg});

  MessageView view{packet.data(), static_cast<uint32_t>(packet.size()), 1000,
                   0};
  NormalizedMessage output[1];

  size_t parsed = parser.parse(view, output, 1);

  assert(parsed == 1);
  assert(output[0].type == NormalizedMessage::Type::SYSTEM_EVENT);
  assert(output[0].timestamp == 12345678900000ULL);

  std::cout << "✓ System Event test passed\n";
}

// Test 2: Parse Add Order
void test_add_order() {
  ItchParser parser;

  auto msg = ItchMessageBuilder::build_add_order(
      1,                 // stock_locate
      200,               // tracking
      12345678900000ULL, // timestamp
      987654321,         // order_ref
      'B',               // side (Buy)
      100,               // shares
      "AAPL    ",        // stock (padded to 8 chars)
      1500000            // price ($150.00 in 4 decimal places)
  );
  auto packet = ItchMessageBuilder::build_packet({msg});

  MessageView view{packet.data(), static_cast<uint32_t>(packet.size()), 1000,
                   0};
  NormalizedMessage output[1];

  size_t parsed = parser.parse(view, output, 1);

  assert(parsed == 1);
  assert(output[0].type == NormalizedMessage::Type::ORDER_ADD);
  assert(output[0].instrument_id == 1);
  assert(output[0].order_id == 987654321);
  assert(output[0].side == 0); // Buy
  assert(output[0].quantity == 100);
  assert(output[0].price == 1500000);

  std::cout << "✓ Add Order test passed\n";
}

// Test 3: Parse Order Executed
void test_order_executed() {
  ItchParser parser;

  auto msg =
      ItchMessageBuilder::build_order_executed(1,   // stock_locate
                                               300, // tracking
                                               12345678900000ULL, // timestamp
                                               987654321,         // order_ref
                                               50,       // executed_shares
                                               111222333 // match_number
      );
  auto packet = ItchMessageBuilder::build_packet({msg});

  MessageView view{packet.data(), static_cast<uint32_t>(packet.size()), 1000,
                   0};
  NormalizedMessage output[1];

  size_t parsed = parser.parse(view, output, 1);

  assert(parsed == 1);
  assert(output[0].type == NormalizedMessage::Type::ORDER_EXECUTE);
  assert(output[0].order_id == 987654321);
  assert(output[0].quantity == 50);

  std::cout << "✓ Order Executed test passed\n";
}

// Test 4: Parse Order Delete
void test_order_delete() {
  ItchParser parser;

  auto msg =
      ItchMessageBuilder::build_order_delete(1,                 // stock_locate
                                             400,               // tracking
                                             12345678900000ULL, // timestamp
                                             987654321          // order_ref
      );
  auto packet = ItchMessageBuilder::build_packet({msg});

  MessageView view{packet.data(), static_cast<uint32_t>(packet.size()), 1000,
                   0};
  NormalizedMessage output[1];

  size_t parsed = parser.parse(view, output, 1);

  assert(parsed == 1);
  assert(output[0].type == NormalizedMessage::Type::ORDER_DELETE);
  assert(output[0].order_id == 987654321);

  std::cout << "✓ Order Delete test passed\n";
}

// Test 5: Parse Trade
void test_trade() {
  ItchParser parser;

  auto msg = ItchMessageBuilder::build_trade(1,                 // stock_locate
                                             500,               // tracking
                                             12345678900000ULL, // timestamp
                                             987654321,         // order_ref
                                             'S',               // side (Sell)
                                             75,                // shares
                                             "MSFT    ",        // stock
                                             3250000,  // price ($325.00)
                                             555666777 // match_number
  );
  auto packet = ItchMessageBuilder::build_packet({msg});

  MessageView view{packet.data(), static_cast<uint32_t>(packet.size()), 1000,
                   0};
  NormalizedMessage output[1];

  size_t parsed = parser.parse(view, output, 1);

  assert(parsed == 1);
  assert(output[0].type == NormalizedMessage::Type::TRADE);
  assert(output[0].side == 1); // Sell
  assert(output[0].quantity == 75);
  assert(output[0].price == 3250000);

  std::cout << "✓ Trade test passed\n";
}

// Test 6: Parse Multiple Messages in One Packet
void test_multiple_messages() {
  ItchParser parser;

  auto msg1 = ItchMessageBuilder::build_add_order(
      1, 100, 12345678900000ULL, 111, 'B', 100, "AAPL    ", 1500000);
  auto msg2 = ItchMessageBuilder::build_order_executed(
      1, 101, 12345678900100ULL, 111, 50, 999);
  auto msg3 =
      ItchMessageBuilder::build_order_delete(1, 102, 12345678900200ULL, 111);

  auto packet = ItchMessageBuilder::build_packet({msg1, msg2, msg3});

  MessageView view{packet.data(), static_cast<uint32_t>(packet.size()), 1000,
                   0};
  NormalizedMessage output[10];

  size_t parsed = parser.parse(view, output, 10);

  assert(parsed == 3);
  assert(output[0].type == NormalizedMessage::Type::ORDER_ADD);
  assert(output[1].type == NormalizedMessage::Type::ORDER_EXECUTE);
  assert(output[2].type == NormalizedMessage::Type::ORDER_DELETE);

  std::cout << "✓ Multiple Messages test passed\n";
}

// Test 7: Performance Test
void test_performance() {
  ItchParser parser;

  // Create a packet with 100 Add Order messages
  std::vector<std::vector<uint8_t>> messages;
  for (int i = 0; i < 100; i++) {
    messages.push_back(ItchMessageBuilder::build_add_order(
        i % 10, i, 12345678900000ULL + i, 1000000 + i, (i % 2 == 0) ? 'B' : 'S',
        100 + i, "TEST    ", 1000000 + i * 100));
  }

  auto packet = ItchMessageBuilder::build_packet(messages);

  MessageView view{packet.data(), static_cast<uint32_t>(packet.size()), 1000,
                   0};
  NormalizedMessage output[100];

  auto start = std::chrono::high_resolution_clock::now();

  // Parse many times to measure performance
  constexpr int iterations = 10000;
  for (int i = 0; i < iterations; i++) {
    parser.parse(view, output, 100);
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

  double avg_per_packet = static_cast<double>(duration.count()) / iterations;
  double avg_per_message = avg_per_packet / 100;

  std::cout << "✓ Performance test passed\n";
  std::cout << "  Average per packet (100 msgs): " << avg_per_packet << " ns\n";
  std::cout << "  Average per message: " << avg_per_message << " ns\n";
}

// Test 8: Parser Statistics
void test_statistics() {
  ItchParser parser;

  auto msg = ItchMessageBuilder::build_add_order(1, 100, 12345678900000ULL, 111,
                                                 'B', 100, "AAPL    ", 1500000);
  auto packet = ItchMessageBuilder::build_packet({msg});

  MessageView view{packet.data(), static_cast<uint32_t>(packet.size()), 1000,
                   0};
  NormalizedMessage output[1];

  parser.parse(view, output, 1);
  parser.parse(view, output, 1);
  parser.parse(view, output, 1);

  Statistics stats;
  parser.get_stats(stats);

  assert(stats.messages_parsed == 3);

  std::cout << "✓ Statistics test passed\n";
}

int main() {
  std::cout << "Running ITCH 5.0 Parser Tests\n";
  std::cout << "==============================\n\n";

  try {
    test_system_event();
    test_add_order();
    test_order_executed();
    test_order_delete();
    test_trade();
    test_multiple_messages();
    test_performance();
    test_statistics();

    std::cout << "\n All ITCH 5.0 parser tests passed!\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "\n Test failed: " << e.what() << "\n";
    return 1;
  }
}