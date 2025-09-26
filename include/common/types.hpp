#pragma once

#include <cstdint>
#include <chrono>
#include <atomic>

namespace hft {

// Ultra-precise timestamp type
using Timestamp = std::chrono::time_point<std::chrono::high_resolution_clock>;
using Duration = std::chrono::nanoseconds;

// Price and quantity types for market data
using Price = int64_t;  // Fixed-point arithmetic, scaled by 10^8
using Quantity = uint64_t;
using OrderId = uint64_t;
using SymbolId = uint32_t;

// Message types
enum class MessageType : uint8_t {
    TRADE = 1,
    QUOTE = 2,
    ORDER_ADD = 3,
    ORDER_MODIFY = 4,
    ORDER_DELETE = 5,
    MARKET_STATUS = 6,
    HEARTBEAT = 7
};

// Side enumeration
enum class Side : uint8_t {
    BUY = 1,
    SELL = 2
};

// Cache line size for alignment
constexpr size_t CACHE_LINE_SIZE = 64;

// Memory alignment macro
#define CACHE_ALIGNED alignas(CACHE_LINE_SIZE)

// Compiler hints for branch prediction
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

// Force inline for critical path functions
#define FORCE_INLINE __attribute__((always_inline)) inline

// Memory barriers
#define MEMORY_BARRIER() __sync_synchronize()
#define COMPILER_BARRIER() asm volatile("" ::: "memory")

// CPU pause for spin loops
#define CPU_PAUSE() asm volatile("pause\n": : :"memory")

} // namespace hft
