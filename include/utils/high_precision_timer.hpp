#pragma once

#include "common/types.hpp"
#include <x86intrin.h>
#include <chrono>
#include <atomic>

namespace hft {

class HighPrecisionTimer {
private:
    static std::atomic<uint64_t> tsc_frequency_;
    static std::atomic<bool> calibrated_;
    
    static void calibrate_tsc() noexcept;

public:
    // Get current timestamp using TSC (Time Stamp Counter)
    FORCE_INLINE static uint64_t get_tsc() noexcept {
        return __rdtsc();
    }
    
    // Get current timestamp using RDTSCP (serializing)
    FORCE_INLINE static uint64_t get_tsc_serialized() noexcept {
        uint32_t aux;
        return __rdtscp(&aux);
    }
    
    // Convert TSC to nanoseconds
    FORCE_INLINE static uint64_t tsc_to_ns(uint64_t tsc) noexcept {
        if (!calibrated_.load(std::memory_order_acquire)) {
            calibrate_tsc();
        }
        
        const uint64_t freq = tsc_frequency_.load(std::memory_order_acquire);
        return (tsc * 1000000000ULL) / freq;
    }
    
    // Get current time in nanoseconds since epoch
    FORCE_INLINE static uint64_t get_ns() noexcept {
        return tsc_to_ns(get_tsc());
    }
    
    // Get high-resolution timestamp
    FORCE_INLINE static Timestamp get_timestamp() noexcept {
        return Timestamp(Duration(get_ns()));
    }
    
    // Measure latency between two TSC readings
    FORCE_INLINE static Duration measure_latency(uint64_t start_tsc, uint64_t end_tsc) noexcept {
        return Duration(tsc_to_ns(end_tsc - start_tsc));
    }
    
    // Sleep for precise nanosecond duration using busy wait
    static void busy_sleep_ns(uint64_t nanoseconds) noexcept;
    
    // Get TSC frequency in Hz
    static uint64_t get_tsc_frequency() noexcept {
        if (!calibrated_.load(std::memory_order_acquire)) {
            calibrate_tsc();
        }
        return tsc_frequency_.load(std::memory_order_acquire);
    }
};

// RAII latency measurement class
class LatencyMeasurement {
private:
    uint64_t start_tsc_;
    std::atomic<uint64_t>* counter_;
    std::atomic<uint64_t>* total_latency_;

public:
    explicit LatencyMeasurement(std::atomic<uint64_t>* counter = nullptr,
                               std::atomic<uint64_t>* total_latency = nullptr) noexcept
        : start_tsc_(HighPrecisionTimer::get_tsc_serialized())
        , counter_(counter)
        , total_latency_(total_latency) {}
    
    ~LatencyMeasurement() noexcept {
        const uint64_t end_tsc = HighPrecisionTimer::get_tsc_serialized();
        const uint64_t latency_ns = HighPrecisionTimer::tsc_to_ns(end_tsc - start_tsc_);
        
        if (counter_) {
            counter_->fetch_add(1, std::memory_order_relaxed);
        }
        
        if (total_latency_) {
            total_latency_->fetch_add(latency_ns, std::memory_order_relaxed);
        }
    }
    
    // Get current elapsed time without destroying the measurement
    FORCE_INLINE uint64_t get_elapsed_ns() const noexcept {
        const uint64_t current_tsc = HighPrecisionTimer::get_tsc();
        return HighPrecisionTimer::tsc_to_ns(current_tsc - start_tsc_);
    }
};

// Statistics collector for latency measurements
class LatencyStats {
private:
    CACHE_ALIGNED std::atomic<uint64_t> count_{0};
    CACHE_ALIGNED std::atomic<uint64_t> total_latency_{0};
    CACHE_ALIGNED std::atomic<uint64_t> min_latency_{UINT64_MAX};
    CACHE_ALIGNED std::atomic<uint64_t> max_latency_{0};
    
    // Histogram buckets (in nanoseconds)
    static constexpr size_t HISTOGRAM_BUCKETS = 20;
    static constexpr uint64_t BUCKET_RANGES[HISTOGRAM_BUCKETS] = {
        100, 200, 300, 400, 500,           // Sub-microsecond
        1000, 2000, 3000, 4000, 5000,     // 1-5 microseconds
        10000, 20000, 30000, 40000, 50000, // 10-50 microseconds
        100000, 200000, 500000, 1000000,   // 100μs - 1ms
        UINT64_MAX                          // > 1ms
    };
    
    CACHE_ALIGNED std::atomic<uint64_t> histogram_[HISTOGRAM_BUCKETS];

public:
    LatencyStats() {
        for (size_t i = 0; i < HISTOGRAM_BUCKETS; ++i) {
            histogram_[i].store(0, std::memory_order_relaxed);
        }
    }
    
    FORCE_INLINE void record_latency(uint64_t latency_ns) noexcept {
        count_.fetch_add(1, std::memory_order_relaxed);
        total_latency_.fetch_add(latency_ns, std::memory_order_relaxed);
        
        // Update min/max
        uint64_t current_min = min_latency_.load(std::memory_order_relaxed);
        while (latency_ns < current_min && 
               !min_latency_.compare_exchange_weak(current_min, latency_ns,
                                                  std::memory_order_relaxed)) {
            // Retry
        }
        
        uint64_t current_max = max_latency_.load(std::memory_order_relaxed);
        while (latency_ns > current_max && 
               !max_latency_.compare_exchange_weak(current_max, latency_ns,
                                                  std::memory_order_relaxed)) {
            // Retry
        }
        
        // Update histogram
        for (size_t i = 0; i < HISTOGRAM_BUCKETS; ++i) {
            if (latency_ns <= BUCKET_RANGES[i]) {
                histogram_[i].fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }
    
    struct Stats {
        uint64_t count;
        uint64_t avg_latency_ns;
        uint64_t min_latency_ns;
        uint64_t max_latency_ns;
        uint64_t histogram[HISTOGRAM_BUCKETS];
    };
    
    Stats get_stats() const noexcept {
        Stats stats;
        stats.count = count_.load(std::memory_order_acquire);
        
        if (stats.count > 0) {
            stats.avg_latency_ns = total_latency_.load(std::memory_order_acquire) / stats.count;
            stats.min_latency_ns = min_latency_.load(std::memory_order_acquire);
            stats.max_latency_ns = max_latency_.load(std::memory_order_acquire);
        } else {
            stats.avg_latency_ns = 0;
            stats.min_latency_ns = 0;
            stats.max_latency_ns = 0;
        }
        
        for (size_t i = 0; i < HISTOGRAM_BUCKETS; ++i) {
            stats.histogram[i] = histogram_[i].load(std::memory_order_acquire);
        }
        
        return stats;
    }
    
    void reset() noexcept {
        count_.store(0, std::memory_order_release);
        total_latency_.store(0, std::memory_order_release);
        min_latency_.store(UINT64_MAX, std::memory_order_release);
        max_latency_.store(0, std::memory_order_release);
        
        for (size_t i = 0; i < HISTOGRAM_BUCKETS; ++i) {
            histogram_[i].store(0, std::memory_order_release);
        }
    }
};

} // namespace hft
