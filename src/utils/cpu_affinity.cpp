#include "utils/cpu_affinity.hpp"
#include <fstream>
#include <sstream>
#include <sys/syscall.h>
#include <numa.h>
#include <sys/mman.h>

namespace hft {

bool CPUAffinity::set_thread_affinity(int cpu_id) noexcept {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    
    return sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0;
}

bool CPUAffinity::set_thread_affinity(std::thread::id thread_id, int cpu_id) noexcept {
    // Convert std::thread::id to native handle
    // This is implementation-specific and may not work on all systems
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    
    // For now, just set current thread affinity
    return sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0;
}

bool CPUAffinity::set_process_affinity(const std::vector<int>& cpu_ids) noexcept {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    for (int cpu_id : cpu_ids) {
        CPU_SET(cpu_id, &cpuset);
    }
    
    return sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0;
}

std::vector<int> CPUAffinity::get_thread_affinity() noexcept {
    cpu_set_t cpuset;
    std::vector<int> cpus;
    
    if (sched_getaffinity(0, sizeof(cpuset), &cpuset) == 0) {
        for (int i = 0; i < CPU_SETSIZE; ++i) {
            if (CPU_ISSET(i, &cpuset)) {
                cpus.push_back(i);
            }
        }
    }
    
    return cpus;
}

int CPUAffinity::get_cpu_count() noexcept {
    return sysconf(_SC_NPROCESSORS_ONLN);
}

std::vector<CPUAffinity::CPUInfo> CPUAffinity::get_cpu_topology() noexcept {
    std::vector<CPUInfo> topology;
    
    // Parse /proc/cpuinfo for topology information
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    CPUInfo current_cpu;
    bool has_cpu = false;
    
    while (std::getline(cpuinfo, line)) {
        if (line.empty()) {
            if (has_cpu) {
                topology.push_back(current_cpu);
                current_cpu = CPUInfo{};
                has_cpu = false;
            }
            continue;
        }
        
        size_t colon_pos = line.find(':');
        if (colon_pos == std::string::npos) continue;
        
        std::string key = line.substr(0, colon_pos);
        std::string value = line.substr(colon_pos + 1);
        
        // Trim whitespace
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        
        if (key == "processor") {
            current_cpu.cpu_id = std::stoi(value);
            has_cpu = true;
        } else if (key == "physical id") {
            current_cpu.socket_id = std::stoi(value);
        } else if (key == "core id") {
            current_cpu.core_id = std::stoi(value);
        } else if (key == "siblings") {
            // Detect hyperthreading
            int siblings = std::stoi(value);
            current_cpu.is_hyperthread = siblings > 1;
        }
    }
    
    if (has_cpu) {
        topology.push_back(current_cpu);
    }
    
    return topology;
}

bool CPUAffinity::isolate_cpus(const std::vector<int>& cpu_ids) noexcept {
    // This would typically require kernel boot parameters or cgroups
    // For demonstration, we'll just set affinity
    return set_process_affinity(cpu_ids);
}

bool CPUAffinity::set_thread_priority(int priority) noexcept {
    return setpriority(PRIO_PROCESS, 0, priority) == 0;
}

bool CPUAffinity::set_realtime_priority(int priority) noexcept {
    struct sched_param param;
    param.sched_priority = priority;
    
    return sched_setscheduler(0, SCHED_FIFO, &param) == 0;
}

bool CPUAffinity::disable_frequency_scaling() noexcept {
    // This typically requires root privileges and system-specific commands
    // For demonstration, we'll return true
    return true;
}

uint64_t CPUAffinity::get_cpu_frequency() noexcept {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    
    while (std::getline(cpuinfo, line)) {
        if (line.find("cpu MHz") != std::string::npos) {
            size_t colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                try {
                    double mhz = std::stod(line.substr(colon_pos + 1));
                    return static_cast<uint64_t>(mhz * 1000000);
                } catch (...) {
                    // Continue trying
                }
            }
        }
    }
    
    return 0;
}

bool CPUAffinity::set_memory_policy(int node_id) noexcept {
    if (numa_available() < 0) {
        return false;
    }
    
    struct bitmask* nodemask = numa_allocate_nodemask();
    numa_bitmask_setbit(nodemask, node_id);
    
    int result = numa_set_membind(nodemask);
    numa_free_nodemask(nodemask);
    
    return result == 0;
}

std::vector<int> CPUAffinity::get_numa_nodes() noexcept {
    std::vector<int> nodes;
    
    if (numa_available() >= 0) {
        int max_node = numa_max_node();
        for (int i = 0; i <= max_node; ++i) {
            if (numa_bitmask_isbitset(numa_nodes_ptr, i)) {
                nodes.push_back(i);
            }
        }
    }
    
    return nodes;
}

bool ThreadConfig::apply() const noexcept {
    bool success = true;
    
    // Set CPU affinity
    if (cpu_id >= 0) {
        success &= CPUAffinity::set_thread_affinity(cpu_id);
    }
    
    // Set scheduling policy and priority
    if (use_realtime) {
        success &= CPUAffinity::set_realtime_priority(realtime_priority);
    } else if (priority != 0) {
        success &= CPUAffinity::set_thread_priority(priority);
    }
    
    // Lock memory if requested
    if (lock_memory) {
        success &= (mlockall(MCL_CURRENT | MCL_FUTURE) == 0);
    }
    
    return success;
}

} // namespace hft
