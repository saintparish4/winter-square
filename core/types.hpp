#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>

namespace hft {
namespace core {

// High-Precision timestamp in nanoseconds since epoch
using Timestamp = uint64_t;

// Get current timestamp with minimal overhead
inline Timestamp get_timestamp() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Message view - zero-copy reference to raw data
struct alignas(64) MessageView {
  const uint8_t *data; // Pointer to message data
  uint32_t length;     // Message length in bytes
  Timestamp timestamp; // Reception timestamp
  uint32_t sequence;   // Sequence number

  MessageView() noexcept
      : data(nullptr), length(0), timestamp(0), sequence(0) {}

  MessageView(const uint8_t *d, uint32_t len, Timestamp ts,
              uint32_t seq) noexcept
      : data(d), length(len), timestamp(ts), sequence(seq) {}

  // Check if message is valid
  bool is_valid() const noexcept { return data != nullptr && length > 0; }
};

// Normalized message - internal canonical format
struct alignas(64) NormalizedMessage {
  enum class Type : uint8_t {
    UNKNOWN = 0,
    TRADE = 1,
    QUOTE = 2,
    ORDER_ADD = 3,
    ORDER_MODIFY = 4,
    ORDER_DELETE = 5,
    ORDER_EXECUTE = 6,
    IMBALANCE = 7,
    SYSTEM_EVENT = 8
  };

  Type type;
  uint64_t instrument_id;    // Internal instrument identifier
  uint64_t order_id;         // Order reference
  int64_t price;             // Price in fixed point (scale: 10000)
  uint64_t quantity;         // Quantity
  uint8_t side;              // 0=buy, 1=sell
  Timestamp timestamp;       // Original exchange timestamp
  Timestamp local_timestamp; // Local reception timestamp
  uint32_t sequence;         // Message sequence

  NormalizedMessage() noexcept
      : type(Type::UNKNOWN), instrument_id(0), order_id(0), price(0),
        quantity(0), side(0), timestamp(0), local_timestamp(0), sequence(0) {}
};

// Configuration constants
namespace config {
constexpr size_t CACHELINE_SIZE = 64;
constexpr size_t PAGE_SIZE = 4096;
constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;

// Network config
constexpr size_t MAX_PACKET_SIZE = 9000; // Jumbo frame
constexpr size_t PACKET_RING_SIZE = 1024 * 16;

// Queue config
constexpr size_t DEFAULT_QUEUE_SIZE = 1024 * 64;

// Thread affinity
constexpr int NETWORK_THREAD_CPU = 2;
constexpr int DISPATCHER_THREAD_CPU = 3;
} // namespace config

// Statistics counters
struct alignas(64) Statistics {
  uint64_t packets_received{0};
  uint64_t packets_dropped{0};
  uint64_t messages_parsed{0};
  uint64_t messages_dispatched{0};
  uint64_t parse_errors{0};
  uint64_t min_latency_ns{UINT64_MAX};
  uint64_t max_latency_ns{0};
  uint64_t total_latency_ns{0};

  void update_latency(uint64_t latency_ns) noexcept {
    if (latency_ns < min_latency_ns)
      min_latency_ns = latency_ns;
    if (latency_ns > max_latency_ns)
      max_latency_ns = latency_ns;
    total_latency_ns += latency_ns;
  }

  double avg_latency_ns() const noexcept {
    return messages_dispatched > 0
               ? static_cast<double>(total_latency_ns) / messages_dispatched
               : 0.0;
  }
};

} // namespace core
} // namespace hft