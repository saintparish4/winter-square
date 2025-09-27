#pragma once

#include <cstdint>
#include <chrono>
#include <atomic>

namespace hft {

    // Ultra-precise timestamp type - using steady_clock for monotonic time
    using Timestamp = std::chrono::time_point<std::chrono::steady_clock>;
    using Duration = std::chrono::nanoseconds;

    // Price and quantity types for market data
    using Price = int64_t; // Fixed point arithmetic, scaled by 10^8
    using Quantity = uint64_t;
    using OrderId = uint64_t;
    using SymbolId = uint32_t;

    // Message sequence number for gap detection
    using SequenceNumber = uint64_t;

    // Message types - consider making this a strong enum class
    enum class MessageType : uint8_t {
        INVALID = 0, // explicitly invalid state
        TRADE = 1,
        QUOTE = 2,
        ORDER_ADD = 3,
        ORDER_MODIFY = 4,
        ORDER_DELETE = 5,
        MARKET_STATUS = 6,
        HEARTBEAT = 7,
        MAX_MESSAGE_TYPE = HEARTBEAT // FOR bounds checking
    };

    // Side enumeration
    enum class Side : uint8_t {
        INVALID = 0, // explicitly invalid state
        BUY = 1,
        SELL = 2 
    };

    // Market status enumeration
    enum class MarketStatus : uint8_t {
        PRE_OPEN = 1,
        OPEN = 2,
        HALTED = 3,
        CLOSED = 4 
    };

    // Hardware-specific optimizations
    constexpr size_t CACHE_LINE_SIZE = 64;
    constexpr size_t PAGE_SIZE = 4096;

    // Memory alignment macros
    #define CACHE_ALIGNED alignas(CACHE_LINE_SIZE)
    #define PAGE_ALIGNED alignas(PAGE_SIZE)

    // Compiler optimization hints
    #ifdef __GNUC__
        #define LIKELY(x) __builtin_expect(!!(x), 1)
        #define UNLIKELY(x) __builtin_expect(!!(x), 0)
        #define FORCE_INLINE __attribute__((always_inline)) inline
        #define NO_INLINE __attribute__((noinline))
        #define PURE_FUNCTION __attribute__((pure))
        #define CONST_FUNCTION __attribute__((const))
        #define HOT_PATH __attribute__((hot))
        #define COLD_PATH __attribute__((cold))
    #else
        #define LIKELY(x) (x)
        #define UNLIKELY(x) (x)
        #define FORCE_INLINE inline
        #define NO_INLINE
        #define PURE_FUNCTION
        #define CONST_FUNCTION
        #define HOT_PATH
        #define COLD_PATH
    #endif

    // Memory ordering and barriers
    #define MEMORY_BARRIER() std::atomic_thread_fence(std::memory_order_seq_cst)
    #define ACQUIRE_BARRIER() std::atomic_thread_fence(std::memory_order_acquire)
    #define RELEASE_BARRIER() std::atomic_thread_fence(std::memory_order_release)
    #define COMPILER_BARRIER() asm volatile("" ::: "memory")

    // CPU-specific optimizations
    #ifdef __x86_64__
        #define CPU_PAUSE() asm volatile("pause\n": : : "memory")
        #define PREFETCH_READ(addr) __builtin_prefetch(addr, 0, 3)
        #define PREFETCH_WRITE(addr) __builtin_prefetch(addr, 1, 3)
    #else
        #define CPU_PAUSE()
        #define PREFETCH_READ(addr)
        #define PREFETCH_WRITE(addr)
    #endif

    // Constants
    namespace constants {
        constexpr size_t DEFAULT_QUEUE_SIZE = 65536;
        constexpr size_t MAX_SYMBOLS = 16384;
        constexpr uint32_t PRICE_SCALE_FACTOR = 100000000;  // 10^8 for price scaling
        constexpr uint64_t INVALID_ORDER_ID = 0;
        constexpr uint32_t INVALID_SYMBOL_ID = 0; 
    }

    // ERROR CODES FOR FAST ERROR HANDLING
    enum class ErrorCode : uint8_t {
        SUCCESS = 0,
        INVALID_MESSAGE = 1,
        SEQUENCE_GAP = 2,
        UNKNOWN_SYMBOL = 3,
        MEMORY_ERROR = 4,
        NETWORK_ERROR = 5, 
    };

    // Utility functions marked for inlining
    FORCE_INLINE CONST_FUNCTION bool is_valid_message_type(MessageType type) noexcept {
        return type > MessageType::INVALID && type <= MessageType::MAX_MESSAGE_TYPE; 
    }

    FORCE_INLINE CONST_FUNCTION bool is_valid_side(Side side) noexcept {
        return side == Side::BUY || side == Side::SELL; 
    }

    FORCE_INLINE CONST_FUNCTION Price scale_price(double price) noexcept {
        return static_cast<Price>(price * constants::PRICE_SCALE_FACTOR); 
    }

    FORCE_INLINE CONST_FUNCTION double unscale_price(Price price) noexcept {
        return static_cast<double>(price) / constants::PRICE_SCALE_FACTOR; 
    }
} // namespace hft
