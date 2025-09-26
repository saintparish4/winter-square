#pragma once

#include "common/types.hpp"
#include <thread>
#include <vector>
#include <sched.h>
#include <unistd.h>
#include <sys/types.h>

namespace hft {

class CPUAffinity {
public:
    // Set CPU affinity for current thread
    static bool set_thread_affinity(int cpu_id) noexcept;
    
    // Set CPU affinity for specific thread
    static bool set_thread_affinity(std::thread::id thread_id, int cpu_id) noexcept;
    
    // Set CPU affinity for process
    static bool set_process_affinity(const std::vector<int>& cpu_ids) noexcept;
    
    // Get current thread's CPU affinity
    static std::vector<int> get_thread_affinity() noexcept;
    
    // Get number of available CPUs
    static int get_cpu_count() noexcept;
    
    // Get CPU topology information
    struct CPUInfo {
        int cpu_id;
        int socket_id;
        int core_id;
        int thread_id;
        bool is_hyperthread;
    };
    
    static std::vector<CPUInfo> get_cpu_topology() noexcept;
    
    // Isolate CPUs for real-time processing
    static bool isolate_cpus(const std::vector<int>& cpu_ids) noexcept;
    
    // Set thread priority
    static bool set_thread_priority(int priority) noexcept; // -20 to 19
    
    // Set real-time scheduling policy
    static bool set_realtime_priority(int priority = 99) noexcept; // 1-99
    
    // Disable frequency scaling for performance
    static bool disable_frequency_scaling() noexcept;
    
    // Get current CPU frequency
    static uint64_t get_cpu_frequency() noexcept;
    
    // Memory affinity (NUMA)
    static bool set_memory_policy(int node_id) noexcept;
    static std::vector<int> get_numa_nodes() noexcept;
    
private:
    static bool parse_cpu_info(const char* filename, std::vector<CPUInfo>& info) noexcept;
};

// RAII CPU affinity manager
class ScopedCPUAffinity {
private:
    std::vector<int> original_affinity_;
    bool restore_on_destroy_;

public:
    explicit ScopedCPUAffinity(int cpu_id, bool restore = true) noexcept
        : restore_on_destroy_(restore) {
        if (restore_on_destroy_) {
            original_affinity_ = CPUAffinity::get_thread_affinity();
        }
        CPUAffinity::set_thread_affinity(cpu_id);
    }
    
    ~ScopedCPUAffinity() noexcept {
        if (restore_on_destroy_ && !original_affinity_.empty()) {
            CPUAffinity::set_process_affinity(original_affinity_);
        }
    }
    
    ScopedCPUAffinity(const ScopedCPUAffinity&) = delete;
    ScopedCPUAffinity& operator=(const ScopedCPUAffinity&) = delete;
};

// Thread configuration for ultra-low latency
struct ThreadConfig {
    int cpu_id = -1;                    // CPU to bind to (-1 = no binding)
    int priority = 0;                   // Thread priority
    bool use_realtime = false;          // Use real-time scheduling
    int realtime_priority = 50;         // Real-time priority (1-99)
    size_t stack_size = 8 * 1024 * 1024; // Stack size in bytes
    bool lock_memory = false;           // Lock thread memory (mlockall)
    
    // Apply configuration to current thread
    bool apply() const noexcept;
};

// Helper function to configure thread for HFT
FORCE_INLINE bool configure_hft_thread(int cpu_id) noexcept {
    ThreadConfig config;
    config.cpu_id = cpu_id;
    config.use_realtime = true;
    config.realtime_priority = 99;
    config.lock_memory = true;
    
    return config.apply();
}

} // namespace hft
