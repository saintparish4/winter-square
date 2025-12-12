#include "../core/core_engine.hpp"
#include "../protocols/itch50/itch50_parser.hpp"
#include <atomic>
#include <csignal>
#include <iomanip>
#include <iostream>


using namespace hft::core;
using namespace hft::protocols::itch50;

std::atomic<bool> running{true};

void signal_handler(int signal) {
  (void)signal;
  running.store(false);
}

// Order book subscriber that tracks orders for a specific instrument
class OrderBookSubscriber : public ISubscriber {
public:
  explicit OrderBookSubscriber(uint64_t instrument_id)
      : instrument_id_(instrument_id), message_count_(0), add_count_(0),
        execute_count_(0), cancel_count_(0), delete_count_(0), trade_count_(0) {
  }

  bool on_message(const NormalizedMessage &msg) noexcept override {
    // Filter by instrument
    if (msg.instrument_id != instrument_id_ && instrument_id_ != 0) {
      return true;
    }

    message_count_++;

    switch (msg.type) {
    case NormalizedMessage::Type::ORDER_ADD:
      add_count_++;
      if (add_count_ % 1000 == 0) {
        print_order(msg, "ADD");
      }
      break;

    case NormalizedMessage::Type::ORDER_EXECUTE:
      execute_count_++;
      if (execute_count_ % 1000 == 0) {
        print_order(msg, "EXECUTE");
      }
      break;

    case NormalizedMessage::Type::ORDER_MODIFY:
      cancel_count_++;
      break;

    case NormalizedMessage::Type::ORDER_DELETE:
      delete_count_++;
      break;

    case NormalizedMessage::Type::TRADE:
      trade_count_++;
      if (trade_count_ % 100 == 0) {
        print_trade(msg);
      }
      break;

    default:
      break;
    }

    return true;
  }

  const char *name() const noexcept override { return "OrderBookSubscriber"; }

  void shutdown() override {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "Order Book Statistics\n";
    std::cout << std::string(60, '=') << "\n";
    std::cout << "Total messages: " << message_count_ << "\n";
    std::cout << "  Add orders:   " << add_count_ << "\n";
    std::cout << "  Executions:   " << execute_count_ << "\n";
    std::cout << "  Cancels:      " << cancel_count_ << "\n";
    std::cout << "  Deletes:      " << delete_count_ << "\n";
    std::cout << "  Trades:       " << trade_count_ << "\n";
    std::cout << std::string(60, '=') << "\n";
  }

private:
  void print_order(const NormalizedMessage &msg, const char *action) const {
    std::cout << "[" << action << "] "
              << "Instrument: " << msg.instrument_id
              << " Order: " << msg.order_id
              << " Side: " << (msg.side == 0 ? "BUY" : "SELL")
              << " Qty: " << msg.quantity << " Price: $" << std::fixed
              << std::setprecision(4) << (msg.price / 10000.0) << "\n";
  }

  void print_trade(const NormalizedMessage &msg) const {
    std::cout << "[TRADE] "
              << "Instrument: " << msg.instrument_id
              << " Side: " << (msg.side == 0 ? "BUY" : "SELL")
              << " Qty: " << msg.quantity << " Price: $" << std::fixed
              << std::setprecision(4) << (msg.price / 10000.0) << "\n";
  }

  uint64_t instrument_id_;
  uint64_t message_count_;
  uint64_t add_count_;
  uint64_t execute_count_;
  uint64_t cancel_count_;
  uint64_t delete_count_;
  uint64_t trade_count_;
};

// Statistics subscriber
class StatisticsSubscriber : public ISubscriber {
public:
  StatisticsSubscriber() : last_print_(std::chrono::steady_clock::now()) {}

  bool on_message(const NormalizedMessage &msg) noexcept override {
    stats_[msg.type]++;

    // Print stats every 5 seconds
    auto now = std::chrono::steady_clock::now();
    if (now - last_print_ >= std::chrono::seconds(5)) {
      print_stats();
      last_print_ = now;
    }

    return true;
  }

  const char *name() const noexcept override { return "StatisticsSubscriber"; }

private:
  void print_stats() const {
    uint64_t total = 0;
    for (const auto &kv : stats_) {
      total += kv.second;
    }

    std::cout << "\n--- Stats (last 5s) ---\n";
    std::cout << "Total: " << total << " messages\n";

    for (const auto &kv : stats_) {
      const char *type_name = get_type_name(kv.first);
      double pct = total > 0 ? (100.0 * kv.second / total) : 0.0;
      std::cout << "  " << std::setw(15) << type_name << ": " << std::setw(8)
                << kv.second << " (" << std::fixed << std::setprecision(1)
                << pct << "%)\n";
    }
    std::cout << std::string(30, '-') << "\n";
  }

  const char *get_type_name(NormalizedMessage::Type type) const {
    switch (type) {
    case NormalizedMessage::Type::UNKNOWN:
      return "Unknown";
    case NormalizedMessage::Type::TRADE:
      return "Trade";
    case NormalizedMessage::Type::QUOTE:
      return "Quote";
    case NormalizedMessage::Type::ORDER_ADD:
      return "Add Order";
    case NormalizedMessage::Type::ORDER_MODIFY:
      return "Modify Order";
    case NormalizedMessage::Type::ORDER_DELETE:
      return "Delete Order";
    case NormalizedMessage::Type::ORDER_EXECUTE:
      return "Execute Order";
    case NormalizedMessage::Type::IMBALANCE:
      return "Imbalance";
    case NormalizedMessage::Type::SYSTEM_EVENT:
      return "System Event";
    default:
      return "Other";
    }
  }

  mutable std::unordered_map<NormalizedMessage::Type, uint64_t> stats_;
  mutable std::chrono::steady_clock::time_point last_print_;
};

int main(int argc, char **argv) {
  std::cout << "HFT Core - ITCH 5.0 Example\n";
  std::cout << "============================\n\n";

  // Install signal handler
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  // Parse command line arguments
  std::string multicast_group = argc > 1 ? argv[1] : "233.54.12.1";
  int port = argc > 2 ? std::atoi(argv[2]) : 20000;
  uint64_t instrument_filter = argc > 3 ? std::atoll(argv[3]) : 0;

  std::cout << "Configuration:\n";
  std::cout << "  Multicast Group: " << multicast_group << "\n";
  std::cout << "  Port: " << port << "\n";
  if (instrument_filter > 0) {
    std::cout << "  Instrument Filter: " << instrument_filter << "\n";
  } else {
    std::cout << "  Instrument Filter: ALL\n";
  }
  std::cout << "\n";

  // Configure core with ITCH parser
  CoreConfig config;
  config.network.multicast_group = multicast_group;
  config.network.port = port;
  config.network_thread_cpu = 2;
  config.parser_thread_cpu = 3;
  config.dispatcher_thread_cpu = 4;
  config.max_messages_per_packet = 100; // ITCH packets can have many messages

  CoreEngine engine(config);

  // Set ITCH 5.0 parser
  engine.set_parser(std::make_unique<ItchParser>());

  // Add subscribers
  engine.add_subscriber(
      std::make_unique<OrderBookSubscriber>(instrument_filter));
  engine.add_subscriber(std::make_unique<StatisticsSubscriber>());

  // Initialize
  if (!engine.initialize()) {
    std::cerr << "Failed to initialize core engine\n";
    return 1;
  }

  std::cout << "Listening for ITCH 5.0 messages...\n";
  std::cout << "Press Ctrl+C to stop\n\n";

  // Start engine
  engine.start();

  // Wait for signal
  while (running.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Print engine stats every 10 seconds
    static int counter = 0;
    if (++counter % 10 == 0) {
      auto stats = engine.get_stats();
      std::cout << "\n[Engine Stats] "
                << "Packets: " << stats.packets_received
                << " Parsed: " << stats.messages_parsed
                << " Dispatched: " << stats.messages_dispatched
                << " Dropped: " << stats.packets_dropped
                << " Errors: " << stats.parse_errors
                << " Latency: " << stats.avg_latency_ns() << "ns"
                << "\n\n";
    }
  }

  std::cout << "\nShutting down...\n";
  engine.stop();

  // Final stats
  auto final_stats = engine.get_stats();
  std::cout << "\n" << std::string(60, '=') << "\n";
  std::cout << "Final Engine Statistics\n";
  std::cout << std::string(60, '=') << "\n";
  std::cout << "Packets received:    " << final_stats.packets_received << "\n";
  std::cout << "Messages parsed:     " << final_stats.messages_parsed << "\n";
  std::cout << "Messages dispatched: " << final_stats.messages_dispatched
            << "\n";
  std::cout << "Packets dropped:     " << final_stats.packets_dropped << "\n";
  std::cout << "Parse errors:        " << final_stats.parse_errors << "\n";
  std::cout << "Min latency:         " << final_stats.min_latency_ns << "ns\n";
  std::cout << "Max latency:         " << final_stats.max_latency_ns << "ns\n";
  std::cout << "Avg latency:         " << final_stats.avg_latency_ns()
            << "ns\n";
  std::cout << std::string(60, '=') << "\n";

  return 0;
}