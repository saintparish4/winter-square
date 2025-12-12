#include "../protocols/itch50/itch50_messages.hpp"
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#define SOCKET_INVALID INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define SOCKET_INVALID (-1)
#endif

using namespace hft::protocols::itch50;

// Helper to write big-endian values
void write_u16_be(uint8_t *dest, uint16_t value) {
  dest[0] = (value >> 8) & 0xFF;
  dest[1] = value & 0xFF;
}

void write_u32_be(uint8_t *dest, uint32_t value) {
  dest[0] = (value >> 24) & 0xFF;
  dest[1] = (value >> 16) & 0xFF;
  dest[2] = (value >> 8) & 0xFF;
  dest[3] = value & 0xFF;
}

void write_u48_be(uint8_t *dest, uint64_t value) {
  dest[0] = (value >> 40) & 0xFF;
  dest[1] = (value >> 32) & 0xFF;
  dest[2] = (value >> 24) & 0xFF;
  dest[3] = (value >> 16) & 0xFF;
  dest[4] = (value >> 8) & 0xFF;
  dest[5] = value & 0xFF;
}

void write_u64_be(uint8_t *dest, uint64_t value) {
  dest[0] = (value >> 56) & 0xFF;
  dest[1] = (value >> 48) & 0xFF;
  dest[2] = (value >> 40) & 0xFF;
  dest[3] = (value >> 32) & 0xFF;
  dest[4] = (value >> 24) & 0xFF;
  dest[5] = (value >> 16) & 0xFF;
  dest[6] = (value >> 8) & 0xFF;
  dest[7] = value & 0xFF;
}

// Generate realistic stock symbols
const char *STOCKS[] = {"AAPL    ", "MSFT    ", "GOOGL   ", "AMZN    ",
                        "TSLA    ", "NVDA    ", "META    ", "NFLX    ",
                        "AMD     ", "INTC    "};
constexpr int NUM_STOCKS = sizeof(STOCKS) / sizeof(STOCKS[0]);

// Build Add Order message (38 bytes with 8-byte timestamp to match parser)
// Layout: stock_locate(2) + tracking(2) + timestamp(8) + type(1) + order_ref(8)
// + side(1) + shares(4) + stock(8) + price(4)
constexpr size_t ADD_ORDER_SIZE = 38;
std::vector<uint8_t> build_add_order(uint16_t stock_locate, uint16_t tracking,
                                     uint64_t timestamp, uint64_t order_ref,
                                     char side, uint32_t shares,
                                     const char *stock, uint32_t price) {
  std::vector<uint8_t> msg(ADD_ORDER_SIZE);
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

// Build Order Executed message (33 bytes with 8-byte timestamp to match parser)
// Layout: stock_locate(2) + tracking(2) + timestamp(8) + type(1) + order_ref(8)
// + shares(4) + match_number(8)
constexpr size_t ORDER_EXECUTED_SIZE = 33;
std::vector<uint8_t> build_order_executed(uint16_t stock_locate,
                                          uint16_t tracking, uint64_t timestamp,
                                          uint64_t order_ref, uint32_t shares,
                                          uint64_t match_number) {
  std::vector<uint8_t> msg(ORDER_EXECUTED_SIZE);
  write_u16_be(&msg[0], stock_locate);
  write_u16_be(&msg[2], tracking);
  write_u64_be(&msg[4], timestamp);
  msg[12] = 'E';
  write_u64_be(&msg[13], order_ref);
  write_u32_be(&msg[21], shares);
  write_u64_be(&msg[25], match_number);
  return msg;
}

// Build Trade message (46 bytes with 8-byte timestamp to match parser)
// Layout: stock_locate(2) + tracking(2) + timestamp(8) + type(1) + order_ref(8)
// + side(1) + shares(4) + stock(8) + price(4) + match_number(8)
constexpr size_t TRADE_SIZE = 46;
std::vector<uint8_t> build_trade(uint16_t stock_locate, uint16_t tracking,
                                 uint64_t timestamp, uint64_t order_ref,
                                 char side, uint32_t shares, const char *stock,
                                 uint32_t price, uint64_t match_number) {
  std::vector<uint8_t> msg(TRADE_SIZE);
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

// Build packet with message length headers
std::vector<uint8_t>
build_packet(const std::vector<std::vector<uint8_t>> &messages) {
  std::vector<uint8_t> packet;
  for (const auto &msg : messages) {
    uint16_t length = msg.size() + 2;
    packet.push_back((length >> 8) & 0xFF);
    packet.push_back(length & 0xFF);
    packet.insert(packet.end(), msg.begin(), msg.end());
  }
  return packet;
}

int main(int argc, char **argv) {
#ifdef _WIN32
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    std::cerr << "WSAStartup failed\n";
    return 1;
  }
#endif

  std::cout << "ITCH 5.0 Message Generator\n";
  std::cout << "===========================\n\n";

  // Parse arguments
  const char *multicast_group = argc > 1 ? argv[1] : "233.54.12.1";
  int port = argc > 2 ? std::atoi(argv[2]) : 20000;
  int rate = argc > 3 ? std::atoi(argv[3]) : 1000; // packets per second
  int msgs_per_packet = argc > 4 ? std::atoi(argv[4]) : 10;

  std::cout << "Configuration:\n";
  std::cout << "  Multicast Group: " << multicast_group << "\n";
  std::cout << "  Port: " << port << "\n";
  std::cout << "  Packet Rate: " << rate << " packets/sec\n";
  std::cout << "  Messages/Packet: " << msgs_per_packet << "\n";
  std::cout << "  Message Rate: " << (rate * msgs_per_packet)
            << " messages/sec\n\n";

  // Create socket
  socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock == SOCKET_INVALID) {
    std::cerr << "Failed to create socket\n";
    return 1;
  }

  // Set multicast TTL
  unsigned char ttl = 1;
#ifdef _WIN32
  setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL,
             reinterpret_cast<const char *>(&ttl), sizeof(ttl));
#else
  setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
#endif

  // Setup destination address
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, multicast_group, &addr.sin_addr);

  // Random number generator
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> stock_dist(0, NUM_STOCKS - 1);
  std::uniform_int_distribution<> side_dist(0, 1);
  std::uniform_int_distribution<> shares_dist(100, 10000);
  std::uniform_int_distribution<> price_dist(500000, 5000000); // $50 - $500
  std::uniform_int_distribution<> msg_type_dist(0,
                                                2); // 0=Add, 1=Execute, 2=Trade

  // State
  uint64_t sequence = 0;
  uint64_t order_id_counter = 1000000;
  uint64_t match_number_counter = 1;
  uint64_t packets_sent = 0;

  // Timing
  const auto interval = std::chrono::microseconds(1000000 / rate);
  auto next_send = std::chrono::steady_clock::now();

  std::cout << "Sending ITCH 5.0 messages... (Ctrl+C to stop)\n\n";

  while (true) {
    // Get current timestamp (nanoseconds since midnight)
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    uint64_t timestamp =
        nanos % (24ULL * 60 * 60 * 1000000000ULL); // Wrap at midnight

    // Generate messages for this packet
    std::vector<std::vector<uint8_t>> messages;
    for (int i = 0; i < msgs_per_packet; i++) {
      int stock_idx = stock_dist(gen);
      uint16_t stock_locate = stock_idx;
      const char *stock = STOCKS[stock_idx];
      char side = side_dist(gen) ? 'B' : 'S';
      uint32_t shares = shares_dist(gen);
      uint32_t price = price_dist(gen);

      int msg_type = msg_type_dist(gen);

      if (msg_type == 0) {
        // Add Order
        messages.push_back(build_add_order(stock_locate, sequence++, timestamp,
                                           order_id_counter++, side, shares,
                                           stock, price));
      } else if (msg_type == 1) {
        // Order Executed
        messages.push_back(build_order_executed(
            stock_locate, sequence++, timestamp,
            order_id_counter - 1000, // Reference older order
            shares / 2, match_number_counter++));
      } else {
        // Trade
        messages.push_back(build_trade(stock_locate, sequence++, timestamp,
                                       order_id_counter++, side, shares, stock,
                                       price, match_number_counter++));
      }
    }

    // Build packet
    auto packet = build_packet(messages);

    // Send packet
#ifdef _WIN32
    int sent = sendto(sock, reinterpret_cast<const char *>(packet.data()),
                      static_cast<int>(packet.size()), 0,
                      reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
#else
    ssize_t sent =
        sendto(sock, packet.data(), packet.size(), 0,
               reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
#endif

    if (sent < 0) {
      std::cerr << "Send failed\n";
      break;
    }

    packets_sent++;

    // Print progress
    if (packets_sent % 1000 == 0) {
      uint64_t total_messages = packets_sent * msgs_per_packet;
      std::cout << "Sent " << packets_sent << " packets (" << total_messages
                << " messages)\n";
    }

    // Rate limiting
    next_send += interval;
    std::this_thread::sleep_until(next_send);
  }

#ifdef _WIN32
  closesocket(sock);
  WSACleanup();
#else
  close(sock);
#endif
  return 0;
}