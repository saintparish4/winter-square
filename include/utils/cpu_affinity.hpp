#pragma once

#include "common/types.hpp"
#include <thread>
#include <vector>
#include <string>
#include <fstream>
#include <atomic>

#ifdef __linux__
#include <sched.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <numa.h>
#include <pthread.h>
#include <errno.h>
#include <cstring>
#endif

namespace hft {

class CPUAffinity {
public:
    // Set CPU affinity for current thread
    static bool set_thread_affinity(int cpu_id) noexcept {
#ifdef __linux__
        if (cpu_id < 0 || cpu_id >= get_cpu_count()) {
            return false;
        }
        
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);
        
        return sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0;
#else
        (void)cpu_id;
        return false;
#endif
    }
    
    // Set CPU affinity for specific thread
    static bool set_thread_affinity(std::thread::id thread_id, int cpu_id) noexcept {
#ifdef __linux__
        if (cpu_id < 0 || cpu_id >= get_cpu_count()) {
            return false;
        }
        
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);
        
        // Convert std::thread::id to pthread_t
        std::thread::native_handle_type native_handle = 
            *reinterpret_cast<const pthread_t*>(&thread_id);
        
        return pthread_setaffinity_np(native_handle, sizeof(cpuset), &cpuset) == 0;
#else
        (void)thread_id; (void)cpu_id;
        return false;
#endif
    }
    
    // Set CPU affinity for process or multiple CPUs
    static bool set_process_affinity(const std::vector<int>& cpu_ids) noexcept {
#ifdef __linux__
        if (cpu_ids.empty()) return false;
        
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        
        for (int cpu_id : cpu_ids) {
            if (cpu_id >= 0 && cpu_id < get_cpu_count()) {
                CPU_SET(cpu_id, &cpuset);
            }
        }
        
        return sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0;
#else
        (void)cpu_ids;
        return false;
#endif
    }
    
    // Get current thread's CPU affinity
    static std::vector<int> get_thread_affinity() noexcept {
        std::vector<int> cpu_list;
        
#ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        
        if (sched_getaffinity(0, sizeof(cpuset), &cpuset) == 0) {
            for (int i = 0; i < CPU_SETSIZE; ++i) {
                if (CPU_ISSET(i, &cpuset)) {
                    cpu_list.push_back(i);
                }
            }
        }
#endif
        
        return cpu_list;
    }
    
    // Get number of available CPUs
    static int get_cpu_count() noexcept {
#ifdef __linux__
        static std::atomic<int> cached_count{-1};
        
        int count = cached_count.load(std::memory_order_relaxed);
        if (count == -1) {
            count = sysconf(_SC_NPROCESSORS_ONLN);
            cached_count.store(count, std::memory_order_relaxed);
        }
        
        return count;
#else
        return std::thread::hardware_concurrency();
#endif
    }
    
    // Get current CPU that thread is running on
    static int get_current_cpu() noexcept {
#ifdef __linux__
        return sched_getcpu();
#else
        return -1;
#endif
    }
    
    // CPU topology information
    struct CPUInfo {
        int cpu_id;
        int socket_id;
        int core_id;
        int thread_id;
        bool is_hyperthread;
        uint64_t cache_size_l1;
        uint64_t cache_size_l2;
        uint64_t cache_size_l3;
        
        CPUInfo() : cpu_id(-1), socket_id(-1), core_id(-1), thread_id(-1)
                  , is_hyperthread(false), cache_size_l1(0), cache_size_l2(0), cache_size_l3(0) {}
    };
    
    static std::vector<CPUInfo> get_cpu_topology() noexcept {
        std::vector<CPUInfo> topology;
        
#ifdef __linux__
        static std::atomic<bool> cached{false};
        static std::vector<CPUInfo> cached_topology;
        
        if (!cached.load(std::memory_order_acquire)) {
            parse_cpu_topology(cached_topology);
            cached.store(true, std::memory_order_release);
        }
        
        topology = cached_topology;
#endif
        
        return topology;
    }
    
    // Get CPUs on the same socket/NUMA node
    static std::vector<int> get_socket_cpus(int socket_id) noexcept {
        std::vector<int> cpus;
        auto topology = get_cpu_topology();
        
        for (const auto& cpu_info : topology) {
            if (cpu_info.socket_id == socket_id) {
                cpus.push_back(cpu_info.cpu_id);
            }
        }
        
        return cpus;
    }
    
    // Get physical cores (excluding hyperthreads)
    static std::vector<int> get_physical_cores() noexcept {
        std::vector<int> cores;
        auto topology = get_cpu_topology();
        
        for (const auto& cpu_info : topology) {
            if (!cpu_info.is_hyperthread) {
                cores.push_back(cpu_info.cpu_id);
            }
        }
        
        return cores;
    }
    
    // Set thread priority (nice value: -20 to 19)
    static bool set_thread_priority(int priority) noexcept {
#ifdef __linux__
        if (priority < -20 || priority > 19) {
            return false;
        }
        
        return setpriority(PRIO_PROCESS, 0, priority) == 0;
#else
        (void)priority;
        return false;
#endif
    }
    
    // Set real-time scheduling policy
    static bool set_realtime_priority(int priority = 99) noexcept {
#ifdef __linux__
        if (priority < 1 || priority > 99) {
            return false;
        }
        
        struct sched_param param;
        param.sched_priority = priority;
        
        // Try SCHED_FIFO first, fallback to SCHED_RR
        if (sched_setscheduler(0, SCHED_FIFO, &param) == 0) {
            return true;
        }
        
        return sched_setscheduler(0, SCHED_RR, &param) == 0;
#else
        (void)priority;
        return false;
#endif
    }
    
    // Check if running with real-time priority
    static bool is_realtime() noexcept {
#ifdef __linux__
        int policy = sched_getscheduler(0);
        return policy == SCHED_FIFO || policy == SCHED_RR;
#else
        return false;
#endif
    }
    
    // Lock memory to prevent swapping
    static bool lock_memory() noexcept {
#ifdef __linux__
        return mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
#else
        return false;
#endif
    }
    
    static bool unlock_memory() noexcept {
#ifdef __linux__
        return munlockall() == 0;
#else
        return false;
#endif
    }
    
    // CPU frequency management
    static bool set_cpu_governor(const char* governor) noexcept {
#ifdef __linux__
        std::string path = "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor";
        std::ofstream file(path);
        
        if (file.is_open()) {
            file << governor;
            return file.good();
        }
#else
        (void)governor;
#endif
        return false;
    }
    
    static bool set_performance_mode() noexcept {
        return set_cpu_governor("performance");
    }
    
    static uint64_t get_cpu_frequency(int cpu_id = 0) noexcept {
#ifdef __linux__
        std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu_id) + 
                          "/cpufreq/scaling_cur_freq";
        std::ifstream file(path);
        
        if (file.is_open()) {
            uint64_t freq;
            file >> freq;
            return freq * 1000; // Convert from kHz to Hz
        }
#else
        (void)cpu_id;
#endif
        return 0;
    }
    
    // NUMA memory affinity
    static bool set_memory_policy(int node_id) noexcept {
#ifdef __linux__
        if (numa_available() == -1) {
            return false;
        }
        
        struct bitmask* mask = numa_allocate_nodemask();
        if (!mask) return false;
        
        numa_bitmask_setbit(mask, node_id);
        int result = numa_set_membind(mask);
        numa_free_nodemask(mask);
        
        return result == 0;
#else
        (void)node_id;
        return false;
#endif
    }
    
    static std::vector<int> get_numa_nodes() noexcept {
        std::vector<int> nodes;
        
#ifdef __linux__
        if (numa_available() != -1) {
            int max_nodes = numa_max_node() + 1;
            for (int i = 0; i < max_nodes; ++i) {
                if (numa_bitmask_isbitset(numa_nodes_ptr, i)) {
                    nodes.push_back(i);
                }
            }
        }
#endif
        
        return nodes;
    }
    
    // Get NUMA node for CPU
    static int get_numa_node_for_cpu(int cpu_id) noexcept {
#ifdef __linux__
        if (numa_available() != -1) {
            return numa_node_of_cpu(cpu_id);
        }
#else
        (void)cpu_id;
#endif
        return -1;
    }
    
    // Interrupt affinity management
    static bool set_irq_affinity(int irq, const std::vector<int>& cpu_ids) noexcept {
#ifdef __linux__
        std::string path = "/proc/irq/" + std::to_string(irq) + "/smp_affinity";
        std::ofstream file(path);
        
        if (file.is_open()) {
            // Convert CPU list to hex mask
            uint64_t mask = 0;
            for (int cpu_id : cpu_ids) {
                if (cpu_id >= 0 && cpu_id < 64) {
                    mask |= (1ULL << cpu_id);
                }
            }
            
            file << std::hex << mask;
            return file.good();
        }
#else
        (void)irq; (void)cpu_ids;
#endif
        return false;
    }

private:
    static bool parse_cpu_topology(std::vector<CPUInfo>& topology) noexcept {
#ifdef __linux__
        topology.clear();
        
        int cpu_count = get_cpu_count();
        topology.reserve(cpu_count);
        
        for (int cpu = 0; cpu < cpu_count; ++cpu) {
            CPUInfo info;
            info.cpu_id = cpu;
            
            // Read socket ID
            std::string socket_path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + 
                                     "/topology/physical_package_id";
            std::ifstream socket_file(socket_path);
            if (socket_file.is_open()) {
                socket_file >> info.socket_id;
            }
            
            // Read core ID
            std::string core_path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + 
                                   "/topology/core_id";
            std::ifstream core_file(core_path);
            if (core_file.is_open()) {
                core_file >> info.core_id;
            }
            
            // Read thread siblings to detect hyperthreading
            std::string siblings_path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + 
                                       "/topology/thread_siblings_list";
            std::ifstream siblings_file(siblings_path);
            if (siblings_file.is_open()) {
                std::string siblings;
                std::getline(siblings_file, siblings);
                // If more than one CPU in siblings list, this is a hyperthread
                info.is_hyperthread = siblings.find(',') != std::string::npos;
            }
            
            topology.push_back(info);
        }
        
        return true;
#else
        (void)topology;
        return false;
#endif
    }
};

// RAII CPU affinity manager
class ScopedCPUAffinity {
private:
    std::vector<int> original_affinity_;
    bool restore_on_destroy_;
    bool valid_;

public:
    explicit ScopedCPUAffinity(int cpu_id, bool restore = true) noexcept
        : restore_on_destroy_(restore), valid_(false) {
        
        if (restore_on_destroy_) {
            original_affinity_ = CPUAffinity::get_thread_affinity();
        }
        
        valid_ = CPUAffinity::set_thread_affinity(cpu_id);
    }
    
    explicit ScopedCPUAffinity(const std::vector<int>& cpu_ids, bool restore = true) noexcept
        : restore_on_destroy_(restore), valid_(false) {
        
        if (restore_on_destroy_) {
            original_affinity_ = CPUAffinity::get_thread_affinity();
        }
        
        valid_ = CPUAffinity::set_process_affinity(cpu_ids);
    }
    
    ~ScopedCPUAffinity() noexcept {
        if (restore_on_destroy_ && !original_affinity_.empty()) {
            CPUAffinity::set_process_affinity(original_affinity_);
        }
    }
    
    bool is_valid() const noexcept { return valid_; }
    
    ScopedCPUAffinity(const ScopedCPUAffinity&) = delete;
    ScopedCPUAffinity& operator=(const ScopedCPUAffinity&) = delete;
    ScopedCPUAffinity(ScopedCPUAffinity&&) = delete;
    ScopedCPUAffinity& operator=(ScopedCPUAffinity&&) = delete;
};

// Thread configuration for ultra-low latency
struct ThreadConfig {
    int cpu_id = -1;                    // CPU to bind to (-1 = no binding)
    int priority = 0;                   // Thread priority (-20 to 19)
    bool use_realtime = false;          // Use real-time scheduling
    int realtime_priority = 50;         // Real-time priority (1-99)
    size_t stack_size = 8 * 1024 * 1024; // Stack size in bytes
    bool lock_memory = false;           // Lock thread memory (mlockall)
    bool set_numa_policy = false;       // Set NUMA memory policy
    int numa_node = -1;                 // NUMA node for memory allocation
    
    // Validate configuration
    bool is_valid() const noexcept {
        if (cpu_id >= CPUAffinity::get_cpu_count()) return false;
        if (priority < -20 || priority > 19) return false;
        if (realtime_priority < 1 || realtime_priority > 99) return false;
        if (stack_size < PTHREAD_STACK_MIN) return false;
        return true;
    }
    
    // Apply configuration to current thread
    bool apply() const noexcept {
        if (!is_valid()) return false;
        
        bool success = true;
        
        // Set CPU affinity
        if (cpu_id >= 0) {
            success &= CPUAffinity::set_thread_affinity(cpu_id);
        }
        
        // Set NUMA policy
        if (set_numa_policy && numa_node >= 0) {
            success &= CPUAffinity::set_memory_policy(numa_node);
        } else if (cpu_id >= 0) {
            // Auto-detect NUMA node for CPU
            int auto_numa = CPUAffinity::get_numa_node_for_cpu(cpu_id);
            if (auto_numa >= 0) {
                CPUAffinity::set_memory_policy(auto_numa);
            }
        }
        
        // Set scheduling policy and priority
        if (use_realtime) {
            success &= CPUAffinity::set_realtime_priority(realtime_priority);
        } else if (priority != 0) {
            success &= CPUAffinity::set_thread_priority(priority);
        }
        
        // Lock memory
        if (lock_memory) {
            success &= CPUAffinity::lock_memory();
        }
        
        return success;
    }
};

// Predefined configurations for common HFT scenarios
namespace configs {
    // Ultra-low latency configuration
    inline ThreadConfig ultra_low_latency(int cpu_id) noexcept {
        ThreadConfig config;
        config.cpu_id = cpu_id;
        config.use_realtime = true;
        config.realtime_priority = 99;
        config.lock_memory = true;
        config.set_numa_policy = true;
        return config;
    }
    
    // Market data receiver configuration
    inline ThreadConfig market_data_receiver(int cpu_id) noexcept {
        ThreadConfig config;
        config.cpu_id = cpu_id;
        config.use_realtime = true;
        config.realtime_priority = 95;
        config.lock_memory = true;
        return config;
    }
    
    // Order processing configuration
    inline ThreadConfig order_processor(int cpu_id) noexcept {
        ThreadConfig config;
        config.cpu_id = cpu_id;
        config.use_realtime = true;
        config.realtime_priority = 90;
        config.lock_memory = true;
        return config;
    }
}

// Helper function to configure thread for HFT
FORCE_INLINE bool configure_hft_thread(int cpu_id) noexcept {
    return configs::ultra_low_latency(cpu_id).apply();
}

// CPU isolation helper
class CPUIsolator {
public:
    static bool isolate_cpus_for_hft(const std::vector<int>& cpu_ids) noexcept {
        bool success = true;
        
        // Set performance governor
        success &= CPUAffinity::set_performance_mode();
        
        // Move interrupts away from isolated CPUs
        auto all_cpus = CPUAffinity::get_physical_cores();
        std::vector<int> non_isolated_cpus;
        
        for (int cpu : all_cpus) {
            if (std::find(cpu_ids.begin(), cpu_ids.end(), cpu) == cpu_ids.end()) {
                non_isolated_cpus.push_back(cpu);
            }
        }
        
        // This is a simplified approach - in production you'd need to:
        // 1. Move all IRQs to non-isolated CPUs
        // 2. Configure kernel to avoid isolated CPUs
        // 3. Set CPU isolation in kernel boot parameters
        
        return success;
    }
};

} // namespace hft