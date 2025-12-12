# ❄️ Winter Square

**High-performance, lock-free market data infrastructure for ultra-low latency trading systems.**

---

## Summary

Winter Square is a C++20 header-only library engineered for capturing, parsing, and distributing market data with sub-200 nanosecond latency. Built for quantitative trading firms and HFT systems, it provides the foundational infrastructure layer between raw network feeds and trading strategy engines.

The library implements a pipeline architecture: UDP multicast data flows through a zero-copy ring buffer, gets parsed via a pluggable protocol adapter, and is distributed to subscribers through lock-free queues—all without a single mutex in the hot path.

---

## Features

| Category | Feature |
|----------|---------|
| **Performance** | Sub-100ns average latency (62.89ns with ITCH parser) |
| **Reliability** | Zero packet loss under all tested conditions |
| **Throughput** | 38,000-40,000 packets/second sustained |
| **Protocols** | NASDAQ ITCH 5.0 parser included |
| **Concurrency** | Lock-free SPSC/MPSC queues for thread communication |
| **Memory** | Zero-copy message views, no allocations in hot path |
| **Extensibility** | Pluggable parser and subscriber interfaces |
| **Deployment** | Header-only, single include integration |
| **Determinism** | CPU affinity support for predictable latency |
| **Cache Efficiency** | 64-byte cache-line aligned data structures |

---

## Tech Stack

| Layer | Technology | Purpose |
|-------|------------|---------|
| **Language** | C++20 | Concepts, constexpr, designated initializers |
| **Build System** | CMake 3.16+ | Cross-platform build configuration |
| **Networking** | POSIX Sockets | UDP multicast reception |
| **Concurrency** | `std::atomic` | Lock-free synchronization primitives |
| **Memory Model** | C++11 Memory Ordering | `memory_order_acquire/release` for correctness |
| **Platform** | Linux / WSL2 | POSIX-compliant systems |
| **Compiler** | GCC 10+ / Clang 12+ | Modern C++20 support |
| **Optimization** | `-O3 -march=native -flto` | Aggressive inlining, LTO, CPU-specific tuning |

---

## Tech Tradeoffs

| Decision | Tradeoff | Rationale |
|----------|----------|-----------|
| **Header-only** | Longer compile times vs. easier integration | Single-include deployment; no linking complexity for users |
| **Lock-free queues** | Higher complexity vs. mutex-based | Eliminates contention; critical for sub-microsecond latency targets |
| **SPSC over MPMC** | Limited topology vs. simpler/faster | Most market data pipelines are naturally single-producer; MPSC available when needed |
| **Fixed-size ring buffers** | Memory pre-allocation vs. dynamic sizing | Avoids allocator latency; deterministic memory footprint |
| **POSIX sockets** | No kernel bypass vs. portability | Baseline implementation; DPDK/io_uring on roadmap for production deployments |
| **No external dependencies** | Reimplemented primitives vs. library reuse | Zero dependency overhead; full control over hot-path code |
| **CPU pinning optional** | Requires system tuning vs. out-of-box | Production systems benefit from affinity; dev environments work without |

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
| **Average Latency** | 62.89 ns (ITCH) / 124 ns (echo) / 194 ns (burst) |
| **Packet Loss** | 0% (zero-drop under all test conditions) |
| **Throughput** | 38,000-40,000 packets/second |
| **Parse Success** | 100% |

### Latency by Parser

| Parser | Avg Latency | Notes |
|--------|-------------|-------|
| ITCH 5.0 | **62.89 ns** | Full protocol parsing with field extraction |
| Echo Parser | 124.58 ns | Minimal passthrough |
| High Burst | 194.01 ns | 5M msg/sec target |

> ITCH parser achieves lower latency than echo parser due to better cache behavior with structured message parsing.

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

### Run ITCH 5.0 Example

```bash
# Terminal 1: Start ITCH receiver
./build/examples/itch50_example 233.54.12.1 20000

# Terminal 2: Start ITCH message generator
./build/examples/itch_generator 233.54.12.1 20000
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
├── protocols/
│   └── itch50/
│       ├── itch50_messages.hpp # ITCH 5.0 message definitions
│       └── itch50_parser.hpp   # ITCH 5.0 parser implementation
├── examples/
│   ├── basic_example.cpp       # Basic receiver example
│   ├── udp_sender.cpp          # Test data sender
│   ├── itch50_example.cpp      # ITCH 5.0 receiver example
│   └── itch_generator.cpp      # ITCH 5.0 message generator
├── benchmarks/
│   └── latency_benchmark.cpp
├── tests/
│   ├── test_lockfree_queue.cpp
│   └── test_itch50_parser.cpp
├── docs/
│   ├── BENCHMARK_RESULTS.md    # Core benchmark data
│   └── ITCH_BENMARK_RESULTS.md # ITCH protocol benchmarks
├── build.sh                    # Build script
└── CMakeLists.txt
```

---

## Roadmap

- [x] NASDAQ ITCH 5.0 protocol parser
- [ ] Order Books Implementation

---

## License

Apache License 2.0 — See [LICENSE](LICENSE) for details.
