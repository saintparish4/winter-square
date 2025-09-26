#include "utils/high_precision_timer.hpp"
#include <thread>
#include <fstream>
#include <string>

namespace hft {

std::atomic<uint64_t> HighPrecisionTimer::tsc_frequency_{0};
std::atomic<bool> HighPrecisionTimer::calibrated_{false};

void HighPrecisionTimer::calibrate_tsc() noexcept {
    if (calibrated_.load(std::memory_order_acquire)) {
        return;
    }
    
    // Try to read TSC frequency from /proc/cpuinfo
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    uint64_t cpu_mhz = 0;
    
    while (std::getline(cpuinfo, line)) {
        if (line.find("cpu MHz") != std::string::npos) {
            size_t colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                try {
                    double mhz = std::stod(line.substr(colon_pos + 1));
                    cpu_mhz = static_cast<uint64_t>(mhz * 1000000);
                    break;
                } catch (...) {
                    // Continue trying other methods
                }
            }
        }
    }
    
    // If we couldn't read from /proc/cpuinfo, calibrate manually
    if (cpu_mhz == 0) {
        const auto start_time = std::chrono::high_resolution_clock::now();
        const uint64_t start_tsc = get_tsc();
        
        // Sleep for a short period
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        
        const uint64_t end_tsc = get_tsc();
        const auto end_time = std::chrono::high_resolution_clock::now();
        
        const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end_time - start_time).count();
        const uint64_t tsc_diff = end_tsc - start_tsc;
        
        if (duration > 0) {
            cpu_mhz = (tsc_diff * 1000000000ULL) / duration;
        } else {
            // Fallback to a reasonable default (3 GHz)
            cpu_mhz = 3000000000ULL;
        }
    }
    
    tsc_frequency_.store(cpu_mhz, std::memory_order_release);
    calibrated_.store(true, std::memory_order_release);
}

void HighPrecisionTimer::busy_sleep_ns(uint64_t nanoseconds) noexcept {
    if (nanoseconds == 0) return;
    
    const uint64_t start_tsc = get_tsc();
    const uint64_t target_cycles = (nanoseconds * get_tsc_frequency()) / 1000000000ULL;
    
    while ((get_tsc() - start_tsc) < target_cycles) {
        CPU_PAUSE();
    }
}

// Initialize static constexpr array
constexpr uint64_t LatencyStats::BUCKET_RANGES[LatencyStats::HISTOGRAM_BUCKETS];

} // namespace hft
