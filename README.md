# ❄️ Winter Square

**High-performance, lock-free market data infrastructure for ultra-low latency trading systems.**

A C++20 header-only library providing the core building blocks for capturing, parsing, and distributing market data with sub-200 nanosecond latency.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                          Winter Square                               │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│    ┌──────────────┐     ┌──────────────┐     ┌──────────────────┐   │
│    │ UDP Receiver │────▶│    Parser    │────▶│    Dispatcher    │   │
│    │  (Multicast) │     │  (Pluggable) │     │   (Lock-free)    │   │
│    └──────────────┘     └──────────────┘     └────────┬─────────┘   │
│           │                                           │             │
│           ▼                                           ▼             │
│    ┌──────────────┐                          ┌───────────────┐      │
│    │ Ring Buffer  │                          │  Subscribers  │      │
│    │  (Zero-copy) │                          │  (SPSC Queue) │      │
│    └──────────────┘                          └───────────────┘      │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### Core Components

| Component | Description |
|-----------|-------------|
| **CoreEngine** | Main orchestrator coordinating network, parsing, and distribution |
| **UDPReceiver** | High-performance UDP multicast receiver with ring buffer storage |
| **Dispatcher** | Lock-free message distribution to multiple subscribers |
| **SPSCQueue** | Single-producer single-consumer lock-free queue |
| **MPSCQueue** | Multi-producer single-consumer lock-free queue |
| **IParser** | Pluggable parser interface for protocol adaptation |
| **ISubscriber** | Subscriber interface for message consumption |

---

## Performance

Benchmarked on WSL2 with Release build (`-O3 -march=native -mtune=native -flto`)

| Metric | Result |
|--------|--------|
| **Average Latency** | 124 ns (low load) / 194 ns (burst) |
| **Packet Loss** | 0% (zero-drop under all test conditions) |
| **Throughput** | 38,000-40,000 packets/second |
| **Parse Success** | 100% |

### Latency Under Load

| Load Condition | Avg Latency |
|----------------|-------------|
| Normal Rate | 124.58 ns |
| High Burst (5M msg/sec target) | 194.01 ns |

> Latency increased by only ~70ns despite 40x higher throughput, demonstrating graceful degradation.

---

## Quick Start

### Prerequisites

- C++20 compiler (GCC 10+ or Clang 12+)
- CMake 3.16+
- Linux/WSL2 (POSIX sockets)

### Build

```bash
./build.sh              # Release build
./build.sh --debug      # Debug build with sanitizers
./build.sh --test       # Build and run tests
./build.sh --clean      # Clean build
```

### Run Examples

```bash
# Terminal 1: Start receiver
./build/examples/basic_example

# Terminal 2: Send test data
./build/examples/udp_sender 239.1.1.1 10000 1000
```

---

## Usage

### Basic Integration

```cpp
#include "core/core_engine.hpp"

using namespace hft::core;

// Custom subscriber
class MySubscriber : public ISubscriber {
public:
    bool on_message(const NormalizedMessage& msg) noexcept override {
        // Process message with sub-microsecond budget
        return true;
    }
    
    const char* name() const noexcept override { return "MySubscriber"; }
};

int main() {
    // Configure
    CoreConfig config;
    config.network.multicast_group = "239.1.1.1";
    config.network.port = 10000;
    
    // Create engine
    CoreEngine engine(config);
    engine.add_subscriber(std::make_unique<MySubscriber>());
    
    // Run
    engine.initialize();
    engine.start();
    
    // ... application logic ...
    
    engine.stop();
}
```

### Custom Parser

```cpp
class MyProtocolParser : public IParser {
public:
    size_t parse(const MessageView& raw, 
                 NormalizedMessage* out, 
                 size_t max_out) override {
        // Parse protocol-specific format into normalized messages
        // Return number of messages produced
    }
};

engine.set_parser(std::make_unique<MyProtocolParser>());
```

### CPU Affinity

```cpp
CoreConfig config;
config.network_thread_cpu = 2;     // Pin network thread to CPU 2
config.dispatcher_thread_cpu = 3;  // Pin dispatcher thread to CPU 3
config.parser_thread_cpu = 4;      // Pin parser thread to CPU 4
```

---

## Design Principles

### 🔒 Lock-Free Queues
SPSC and MPSC queues use atomic operations with proper memory ordering for wait-free communication between threads.

### 📐 Cache-Line Alignment
All hot data structures are aligned to 64-byte cache lines (`alignas(64)`) to prevent false sharing.

### 📋 Zero-Copy Message Views
`MessageView` provides read-only access to packet data without memory allocation in the hot path.

### 🧵 Thread Pinning
Network, parser, and dispatcher threads support CPU affinity for deterministic latency.

### 🔌 Extensible Architecture
Pluggable parser and subscriber interfaces allow protocol and handler customization.

---

## Project Structure

```
winter-square/
├── core/
│   ├── core_engine.hpp         # Main orchestrator
│   ├── types.hpp               # Core types and config
│   ├── distribution/
│   │   ├── dispatcher.hpp      # Message distribution
│   │   ├── lockfree_queue.hpp  # SPSC/MPSC queues
│   │   └── subscriber_interface.hpp
│   ├── network/
│   │   └── udp_receiver.hpp    # UDP multicast receiver
│   └── parser/
│       └── parser_interface.hpp
├── examples/
│   ├── basic_example.cpp       # Receiver example
│   └── udp_sender.cpp          # Test data sender
├── benchmarks/
│   └── latency_benchmark.cpp
├── tests/
│   └── test_lockfree_queue.cpp
├── docs/
│   └── BENCHMARK_RESULTS.md    # Detailed benchmark data
├── build.sh                    # Build script
└── CMakeLists.txt
```

---

## Roadmap

- [ ] Kernel bypass networking (DPDK/io_uring)
- [ ] Hardware timestamping
- [ ] Protocol parsers (ITCH, OUCH, FIX)
- [ ] Order book construction
- [ ] FPGA integration path

---

## License

Apache License 2.0 — See [LICENSE](LICENSE) for details.
