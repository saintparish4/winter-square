#pragma once

#include "common/types.hpp"
#include <atomic>
#include <chrono>
#include <thread>

#ifdef __x86_64__
#include <x86intrin.h>
#include <cpuid.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace hft {

class HighPrecisionTimer {
private:
    static std::atomic<uint64_t> tsc_frequency_;
    static std::atomic<bool> calibrated_;
    static std::atomic<bool> tsc_reliable_;
    static std::atomic<uint64_t> calibration_overhead_;
    
    static void calibrate_tsc() noexcept;
    static bool is_tsc_reliable() noexcept;

public:
    // Get current timestamp using TSC (Time Stamp Counter) - fastest but non-serializing
    FORCE_INLINE HOT_PATH static uint64_t get_tsc() noexcept {
#ifdef __x86_64__
        return __rdtsc();
#elif defined(__aarch64__)
        uint64_t val;
        asm volatile("mrs %0, cntvct_el0" : "=r" (val));
        return val;
#else
        return std::chrono::high_resolution_clock::now().time_since_epoch().count();
#endif
    }
    
    // Get current timestamp using RDTSCP (serializing) - slightly slower but more accurate
    FORCE_INLINE HOT_PATH static uint64_t get_tsc_serialized() noexcept {
#ifdef __x86_64__
        uint32_t aux;
        return __rdtscp(&aux);
#elif defined(__aarch64__)
        uint64_t val;
        asm volatile("isb; mrs %0, cntvct_el0" : "=r" (val));
        return val;
#else
        return get_tsc();
#endif
    }
    
    // Get current timestamp with memory fence - ensures no reordering
    FORCE_INLINE static uint64_t get_tsc_fenced() noexcept {
#ifdef __x86_64__
        _mm_lfence();
        uint64_t tsc = __rdtsc();
        _mm_lfence();
        return tsc;
#else
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return get_tsc();
#endif
    }
    
    // Convert TSC to nanoseconds
    FORCE_INLINE HOT_PATH static uint64_t tsc_to_ns(uint64_t tsc) noexcept {
        if (UNLIKELY(!calibrated_.load(std::memory_order_acquire))) {
            calibrate_tsc();
        }
        
        const uint64_t freq = tsc_frequency_.load(std::memory_order_acquire);
        if (UNLIKELY(freq == 0)) {
            return 0;
        }
        
        // Use 128-bit arithmetic to avoid overflow for large TSC values
        return (static_cast<__uint128_t>(tsc) * 1000000000ULL) / freq;
    }
    
    // Convert nanoseconds to TSC
    FORCE_INLINE static uint64_t ns_to_tsc(uint64_t nanoseconds) noexcept {
        if (UNLIKELY(!calibrated_.load(std::memory_order_acquire))) {
            calibrate_tsc();
        }
        
        const uint64_t freq = tsc_frequency_.load(std::memory_order_acquire);
        if (UNLIKELY(freq == 0)) {
            return 0;
        }
        
        return (static_cast<__uint128_t>(nanoseconds) * freq) / 1000000000ULL;
    }
    
    // Get current time in nanoseconds since epoch
    FORCE_INLINE HOT_PATH static uint64_t get_ns() noexcept {
        if (LIKELY(tsc_reliable_.load(std::memory_order_acquire))) {
            return tsc_to_ns(get_tsc());
        } else {
            // Fallback to std::chrono for unreliable TSC
            return std::chrono::steady_clock::now().time_since_epoch().count();
        }
    }
    
    // Get high-resolution timestamp
    FORCE_INLINE HOT_PATH static Timestamp get_timestamp() noexcept {
        return Timestamp(Duration(get_ns()));
    }
    
    // Measure latency between two TSC readings
    FORCE_INLINE CONST_FUNCTION static Duration measure_latency(uint64_t start_tsc, uint64_t end_tsc) noexcept {
        return Duration(tsc_to_ns(end_tsc - start_tsc));
    }
    
    // Sleep for precise nanosecond duration using busy wait
    static void busy_sleep_ns(uint64_t nanoseconds) noexcept {
        if (nanoseconds == 0) return;
        
        const uint64_t start = get_tsc();
        const uint64_t end_tsc = start + ns_to_tsc(nanoseconds);
        
        while (LIKELY(get_tsc() < end_tsc)) {
            CPU_PAUSE();
        }
    }
    
    // Sleep with hybrid approach: yield for long sleeps, busy wait for short ones
    static void hybrid_sleep_ns(uint64_t nanoseconds) noexcept {
        if (nanoseconds == 0) return;
        
        if (nanoseconds > 10000) { // > 10μs, use thread yield
            const auto sleep_duration = std::chrono::nanoseconds(nanoseconds - 5000);
            std::this_thread::sleep_for(sleep_duration);
            
            // Busy wait for the remaining time
            busy_sleep_ns(5000);
        } else {
            busy_sleep_ns(nanoseconds);
        }
    }
    
    // Get TSC frequency in Hz
    static uint64_t get_tsc_frequency() noexcept {
        if (UNLIKELY(!calibrated_.load(std::memory_order_acquire))) {
            calibrate_tsc();
        }
        return tsc_frequency_.load(std::memory_order_acquire);
    }
    
    // Check if TSC is available and reliable
    static bool is_available() noexcept {
        return tsc_reliable_.load(std::memory_order_acquire);
    }
    
    // Get calibration overhead in TSC cycles
    static uint64_t get_calibration_overhead() noexcept {
        return calibration_overhead_.load(std::memory_order_acquire);
    }
    
    // Force recalibration (useful after CPU frequency changes)
    static void recalibrate() noexcept {
        calibrated_.store(false, std::memory_order_release);
        calibrate_tsc();
    }
    
    // Get minimum measurable time difference
    static uint64_t get_resolution_ns() noexcept {
        return tsc_to_ns(1); // 1 TSC tick resolution
    }
    
    // Warm up the timer (call once at startup)
    static void warmup() noexcept {
        // Pre-load calibration
        calibrate_tsc();
        
        // Warm up CPU caches and branch predictors
        for (int i = 0; i < 1000; ++i) {
            volatile uint64_t tsc = get_tsc();
            (void)tsc;
        }
    }
};

// RAII latency measurement class with enhanced features
class LatencyMeasurement {
private:
    uint64_t start_tsc_;
    std::atomic<uint64_t>* counter_;
    std::atomic<uint64_t>* total_latency_;
    const char* label_;
    bool use_serialized_;

public:
    explicit LatencyMeasurement(std::atomic<uint64_t>* counter = nullptr,
                               std::atomic<uint64_t>* total_latency = nullptr,
                               const char* label = nullptr,
                               bool use_serialized = false) noexcept
        : start_tsc_(use_serialized ? HighPrecisionTimer::get_tsc_serialized() : HighPrecisionTimer::get_tsc())
        , counter_(counter)
        , total_latency_(total_latency)
        , label_(label)
        , use_serialized_(use_serialized) {}
    
    ~LatencyMeasurement() noexcept {
        const uint64_t end_tsc = use_serialized_ ? 
            HighPrecisionTimer::get_tsc_serialized() : 
            HighPrecisionTimer::get_tsc();
        
        const uint64_t latency_ns = HighPrecisionTimer::tsc_to_ns(end_tsc - start_tsc_);
        
        if (counter_) {
            counter_->fetch_add(1, std::memory_order_relaxed);
        }
        
        if (total_latency_) {
            total_latency_->fetch_add(latency_ns, std::memory_order_relaxed);
        }
        
        // In debug builds, could log extreme latencies
#ifdef DEBUG
        if (label_ && latency_ns > 1000000) { // > 1ms
            // Log warning about high latency
        }
#endif
    }
    
    // Get current elapsed time without destroying the measurement
    FORCE_INLINE HOT_PATH uint64_t get_elapsed_ns() const noexcept {
        const uint64_t current_tsc = HighPrecisionTimer::get_tsc();
        return HighPrecisionTimer::tsc_to_ns(current_tsc - start_tsc_);
    }
    
    // Get elapsed TSC cycles
    FORCE_INLINE HOT_PATH uint64_t get_elapsed_tsc() const noexcept {
        return HighPrecisionTimer::get_tsc() - start_tsc_;
    }
    
    // Reset the start time
    FORCE_INLINE void restart() noexcept {
        start_tsc_ = use_serialized_ ? 
            HighPrecisionTimer::get_tsc_serialized() : 
            HighPrecisionTimer::get_tsc();
    }
};

// Enhanced statistics collector for latency measurements
class LatencyStats {
private:
    CACHE_ALIGNED std::atomic<uint64_t> count_{0};
    CACHE_ALIGNED std::atomic<uint64_t> total_latency_{0};
    CACHE_ALIGNED std::atomic<uint64_t> min_latency_{UINT64_MAX};
    CACHE_ALIGNED std::atomic<uint64_t> max_latency_{0};
    
    // Enhanced histogram buckets (in nanoseconds) for HFT precision
    static constexpr size_t HISTOGRAM_BUCKETS = 25;
    static constexpr uint64_t BUCKET_RANGES[HISTOGRAM_BUCKETS] = {
        50, 100, 150, 200, 250, 300, 400, 500,     // Sub-microsecond (50ns granularity)
        750, 1000, 1250, 1500, 1750, 2000,         // 1-2 microseconds
        2500, 3000, 4000, 5000,                    // 2-5 microseconds  
        7500, 10000, 15000, 20000,                 // 5-20 microseconds
        50000, 100000, 1000000,                    // 50μs - 1ms
        UINT64_MAX                                  // > 1ms
    };
    
    CACHE_ALIGNED std::atomic<uint64_t> histogram_[HISTOGRAM_BUCKETS];
    
    // Track recent samples for percentile calculation
    static constexpr size_t SAMPLE_BUFFER_SIZE = 10000;
    std::atomic<uint64_t> sample_buffer_[SAMPLE_BUFFER_SIZE];
    CACHE_ALIGNED std::atomic<size_t> sample_index_{0};

public:
    LatencyStats() {
        for (size_t i = 0; i < HISTOGRAM_BUCKETS; ++i) {
            histogram_[i].store(0, std::memory_order_relaxed);
        }
        
        for (size_t i = 0; i < SAMPLE_BUFFER_SIZE; ++i) {
            sample_buffer_[i].store(0, std::memory_order_relaxed);
        }
    }
    
    FORCE_INLINE HOT_PATH void record_latency(uint64_t latency_ns) noexcept {
        count_.fetch_add(1, std::memory_order_relaxed);
        total_latency_.fetch_add(latency_ns, std::memory_order_relaxed);
        
        // Update min/max with compare-exchange loop
        update_atomic_min(min_latency_, latency_ns);
        update_atomic_max(max_latency_, latency_ns);
        
        // Update histogram
        update_histogram(latency_ns);
        
        // Store sample for percentile calculation
        size_t index = sample_index_.fetch_add(1, std::memory_order_relaxed) % SAMPLE_BUFFER_SIZE;
        sample_buffer_[index].store(latency_ns, std::memory_order_relaxed);
    }
    
    struct Stats {
        uint64_t count;
        uint64_t avg_latency_ns;
        uint64_t min_latency_ns;
        uint64_t max_latency_ns;
        uint64_t p50_latency_ns;    // Median
        uint64_t p95_latency_ns;    // 95th percentile
        uint64_t p99_latency_ns;    // 99th percentile
        uint64_t p999_latency_ns;   // 99.9th percentile
        uint64_t histogram[HISTOGRAM_BUCKETS];
        
        // HFT-specific metrics
        uint64_t sub_microsecond_count;    // < 1μs
        uint64_t ultra_low_count;          // < 100ns
        double efficiency_ratio;           // % under target latency
    };
    
    Stats get_stats(uint64_t target_latency_ns = 1000) const noexcept {
        Stats stats = {};
        stats.count = count_.load(std::memory_order_acquire);
        
        if (stats.count > 0) {
            stats.avg_latency_ns = total_latency_.load(std::memory_order_acquire) / stats.count;
            stats.min_latency_ns = min_latency_.load(std::memory_order_acquire);
            stats.max_latency_ns = max_latency_.load(std::memory_order_acquire);
            
            // Calculate percentiles from sample buffer
            calculate_percentiles(stats);
            
            // Calculate HFT-specific metrics
            uint64_t sub_micro = 0, ultra_low = 0, under_target = 0;
            for (size_t i = 0; i < HISTOGRAM_BUCKETS; ++i) {
                stats.histogram[i] = histogram_[i].load(std::memory_order_acquire);
                
                if (BUCKET_RANGES[i] < 1000) {
                    sub_micro += stats.histogram[i];
                }
                if (BUCKET_RANGES[i] < 100) {
                    ultra_low += stats.histogram[i];
                }
                if (BUCKET_RANGES[i] <= target_latency_ns) {
                    under_target += stats.histogram[i];
                }
            }
            
            stats.sub_microsecond_count = sub_micro;
            stats.ultra_low_count = ultra_low;
            stats.efficiency_ratio = static_cast<double>(under_target) / stats.count * 100.0;
        }
        
        return stats;
    }
    
    void reset() noexcept {
        count_.store(0, std::memory_order_release);
        total_latency_.store(0, std::memory_order_release);
        min_latency_.store(UINT64_MAX, std::memory_order_release);
        max_latency_.store(0, std::memory_order_release);
        sample_index_.store(0, std::memory_order_release);
        
        for (size_t i = 0; i < HISTOGRAM_BUCKETS; ++i) {
            histogram_[i].store(0, std::memory_order_release);
        }
        
        for (size_t i = 0; i < SAMPLE_BUFFER_SIZE; ++i) {
            sample_buffer_[i].store(0, std::memory_order_release);
        }
    }
    
    // Get bucket ranges for external analysis
    static const uint64_t* get_bucket_ranges() noexcept {
        return BUCKET_RANGES;
    }
    
    static size_t get_bucket_count() noexcept {
        return HISTOGRAM_BUCKETS;
    }

private:
    FORCE_INLINE void update_atomic_min(std::atomic<uint64_t>& atomic_min, uint64_t value) noexcept {
        uint64_t current_min = atomic_min.load(std::memory_order_relaxed);
        while (value < current_min && 
               !atomic_min.compare_exchange_weak(current_min, value, std::memory_order_relaxed)) {
            CPU_PAUSE();
        }
    }
    
    FORCE_INLINE void update_atomic_max(std::atomic<uint64_t>& atomic_max, uint64_t value) noexcept {
        uint64_t current_max = atomic_max.load(std::memory_order_relaxed);
        while (value > current_max && 
               !atomic_max.compare_exchange_weak(current_max, value, std::memory_order_relaxed)) {
            CPU_PAUSE();
        }
    }
    
    FORCE_INLINE void update_histogram(uint64_t latency_ns) noexcept {
        for (size_t i = 0; i < HISTOGRAM_BUCKETS; ++i) {
            if (latency_ns <= BUCKET_RANGES[i]) {
                histogram_[i].fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }
    
    void calculate_percentiles(Stats& stats) const noexcept {
        // Simplified percentile calculation from sample buffer
        // In production, you might want a more sophisticated approach
        stats.p50_latency_ns = stats.avg_latency_ns;   // Placeholder
        stats.p95_latency_ns = stats.avg_latency_ns * 2; // Placeholder
        stats.p99_latency_ns = stats.avg_latency_ns * 3; // Placeholder
        stats.p999_latency_ns = stats.max_latency_ns;    // Placeholder
    }
};

// Macro for easy latency measurement
#define MEASURE_LATENCY(stats) \
    LatencyMeasurement _latency_measurement_(&stats.count_, &stats.total_latency_)

// Scoped latency measurement with automatic recording
template<typename StatsType>
class ScopedLatencyMeasurement {
private:
    LatencyMeasurement measurement_;
    StatsType& stats_;

public:
    explicit ScopedLatencyMeasurement(StatsType& stats) noexcept 
        : measurement_(), stats_(stats) {}
    
    ~ScopedLatencyMeasurement() noexcept {
        stats_.record_latency(measurement_.get_elapsed_ns());
    }
    
    FORCE_INLINE uint64_t get_elapsed_ns() const noexcept {
        return measurement_.get_elapsed_ns();
    }
};

} // namespace hft