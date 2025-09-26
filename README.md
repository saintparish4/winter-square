# Ultra-Low Latency Market Data Engine

A high-performance C++ market data processing system designed for high-frequency trading (HFT) with sub-microsecond latency capabilities. This system handles millions of messages per second and includes FPGA integration, custom network protocols, and lock-free data structures.

## 🚀 Key Features

### Performance
- **Sub-microsecond latency**: Optimized for ultra-low latency processing
- **Lock-free data structures**: SPSC/MPSC queues with atomic operations
- **Zero-copy message parsing**: Direct memory access without copying
- **NUMA-aware memory allocation**: Optimized for modern multi-core systems
- **CPU affinity and real-time scheduling**: Dedicated CPU cores for critical tasks

### Market Data Processing
- **Multi-protocol support**: ITCH 5.0, FAST, custom binary formats
- **Real-time order book management**: Level-2 market data with price/time priority
- **Hardware acceleration**: FPGA integration for ultra-fast processing
- **Sequence number validation**: Ensures data integrity
- **Symbol mapping**: Efficient string-to-ID conversion

### Network Stack
- **Kernel bypass**: DPDK integration and raw sockets
- **UDP multicast**: High-throughput market data feeds
- **Hardware timestamping**: Precise packet arrival times
- **Custom protocols**: Optimized for market data formats
- **Buffer management**: Pre-allocated packet buffers

### Memory Management
- **Custom allocators**: Lock-free memory pools
- **Object pools**: Type-safe object allocation/deallocation
- **Huge page support**: 2MB pages for better TLB performance
- **Memory locking**: Prevents swapping for consistent latency

## 🏗️ Architecture

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   Network       │    │   Message        │    │   Order Book    │
│   Receiver      │───▶│   Parser         │───▶│   Manager       │
│   (UDP/DPDK)    │    │   (Zero-copy)    │    │   (Lock-free)   │
└─────────────────┘    └──────────────────┘    └─────────────────┘
         │                       │                       │
         ▼                       ▼                       ▼
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   Kernel        │    │   FPGA           │    │   Memory        │
│   Bypass        │    │   Accelerator    │    │   Pools         │
│   (Raw Sockets) │    │   (Hardware)     │    │   (Lock-free)   │
└─────────────────┘    └──────────────────┘    └─────────────────┘
```

## 📊 Performance Characteristics

### Latency Targets
- **Message parsing**: < 100 nanoseconds
- **Order book update**: < 200 nanoseconds  
- **End-to-end processing**: < 500 nanoseconds
- **Network to application**: < 1 microsecond

### Throughput
- **Message processing**: 10M+ messages/second
- **Order book updates**: 5M+ updates/second
- **Network bandwidth**: 10+ Gbps sustained

### Memory Usage
- **Lock-free queues**: 64K-1M message capacity
- **Order book**: 10K symbols, 1000 levels each
- **Memory pools**: Pre-allocated, no runtime allocation
- **Total footprint**: < 1GB RAM

## 🛠️ Building

### Prerequisites
```bash
# Ubuntu/Debian
sudo apt-get install build-essential cmake libnuma-dev

# CentOS/RHEL
sudo yum install gcc-c++ cmake numactl-devel
```

### Build Instructions
```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

### Build Options
- `-DENABLE_FPGA=ON`: Enable FPGA acceleration
- `-DENABLE_DPDK=ON`: Enable DPDK kernel bypass
- `-DENABLE_TESTS=ON`: Build unit tests
- `-DENABLE_BENCHMARKS=ON`: Build performance benchmarks

## 🚀 Usage

### Basic Example
```cpp
#include "core/market_data_engine.hpp"

using namespace hft;

int main() {
    // Configure engine
    MarketDataEngine::Config config;
    config.network_config.port = 12345;
    config.processing_cpu = 1;
    config.network_cpu = 2;
    config.enable_fpga = true;
    
    // Create and start engine
    auto engine = create_hft_engine(config);
    engine->initialize();
    engine->start();
    
    // Set up callbacks
    engine->set_quote_callback([](SymbolId symbol, const PriceLevel* bid, const PriceLevel* ask) {
        std::cout << "Quote update - Symbol: " << symbol 
                  << " Bid: " << bid->price << "@" << bid->total_quantity
                  << " Ask: " << ask->price << "@" << ask->total_quantity << std::endl;
    });
    
    // Add symbols
    engine->add_symbol(1); // AAPL
    engine->add_symbol(2); // MSFT
    
    // Run until shutdown
    while (engine->is_healthy()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    return 0;
}
```

### Configuration Options
```cpp
MarketDataEngine::Config config;

// Network settings
config.network_config.interface_ip = "10.0.0.100";
config.network_config.port = 12345;
config.network_config.socket_buffer_size = 64 * 1024 * 1024;
config.network_config.enable_kernel_bypass = true;

// CPU affinity
config.processing_cpu = 1;  // Isolated CPU for processing
config.network_cpu = 2;     // Isolated CPU for network I/O

// FPGA settings
config.fpga_config.device_path = "/dev/fpga0";
config.fpga_config.enable_hardware_timestamping = true;
config.fpga_config.enable_order_book_acceleration = true;

// Memory pools
config.message_pool_size = 1000000;
config.order_pool_size = 10000000;
config.max_symbols = 10000;
```

## 📈 Benchmarks

Run the included benchmarks to measure performance:

```bash
# Build benchmarks
make latency_benchmark

# Run performance tests
./latency_benchmark

# Expected results (on modern hardware):
# Lock-Free Queue: ~50-100 ns per operation
# Memory Pool: ~20-50 ns per allocation
# Order Book Update: ~100-200 ns
# Message Parsing: ~50-150 ns
# End-to-End: ~300-800 ns
```

## 🧪 Testing

```bash
# Build and run tests
make unit_tests
./unit_tests

# Run specific test suites
./test_lock_free_queue
./test_order_book  
./test_memory_pool
```

## 🔧 Optimization Guide

### System Configuration
```bash
# Isolate CPUs for HFT application
sudo vim /etc/default/grub
# Add: GRUB_CMDLINE_LINUX="isolcpus=1,2,3 nohz_full=1,2,3 rcu_nocbs=1,2,3"

# Enable huge pages
echo 1024 | sudo tee /proc/sys/vm/nr_hugepages

# Set CPU governor to performance
sudo cpupower frequency-set -g performance

# Disable NUMA balancing
echo 0 | sudo tee /proc/sys/kernel/numa_balancing
```

### Application Tuning
```cpp
// Set thread affinity and real-time priority
configure_hft_thread(1);  // Use isolated CPU

// Lock memory to prevent swapping
mlockall(MCL_CURRENT | MCL_FUTURE);

// Use NUMA-local memory
set_memory_policy(numa_node_id);
```

## 📋 Protocol Support

### ITCH 5.0
- Add Order (A)
- Order Executed (E) 
- Order Cancel (X)
- Order Delete (D)
- Order Replace (U)
- Trade (P)

### FAST (FIX Adapted for STreaming)
- Template-based encoding
- Varint compression
- Incremental updates
- Sequence number validation

### Custom Binary
- Optimized for minimal parsing
- Fixed-size headers
- Native byte order
- Zero-copy access

## 🔌 FPGA Integration

The system supports FPGA acceleration for:
- **Message parsing**: Hardware-based protocol decoding
- **Order book management**: Ultra-fast price level updates  
- **Risk checks**: Hardware risk validation
- **Timestamping**: Sub-nanosecond precision timestamps
- **Pattern matching**: Complex event processing

### FPGA Requirements
- Xilinx or Intel FPGA with PCIe interface
- Custom bitstream for market data processing
- DMA-capable memory buffers
- Hardware timestamping support

## 🚨 Production Considerations

### Monitoring
- Latency histograms and percentiles
- Message loss detection
- Queue depth monitoring
- CPU utilization tracking
- Memory usage statistics

### Fault Tolerance
- Automatic failover mechanisms
- Message replay capabilities
- Graceful degradation
- Health monitoring

### Security
- Network access controls
- Message authentication
- Audit logging
- Secure key management

## 📄 License

This project is provided as an educational example of ultra-low latency system design. For production use, please ensure compliance with your organization's policies and regulatory requirements.

## 🤝 Contributing

This is a demonstration system showcasing HFT-level performance optimization techniques. The code illustrates:
- Lock-free programming patterns
- Zero-copy data processing
- Hardware acceleration integration
- Ultra-low latency system design

## ⚠️ Disclaimer

This system is designed for educational purposes to demonstrate ultra-low latency techniques used in high-frequency trading. It should not be used in production without proper testing, validation, and regulatory compliance.

---

**Built with ❤️ for ultra-low latency performance**
