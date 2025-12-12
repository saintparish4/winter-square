#pragma once

#include "../distribution/lockfree_queue.hpp"
#include "../types.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>


#ifdef _WIN32
// Windows stubs for IDE linting - actual build uses WSL/Linux
#define AF_INET 2
#define SOCK_DGRAM 2
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_RCVBUF 8
#define SO_TIMESTAMP 29
#define IPPROTO_IP 0
#define IP_ADD_MEMBERSHIP 35
#define INADDR_ANY 0
struct sockaddr {
  unsigned short sa_family;
  char sa_data[14];
};
struct in_addr {
  uint32_t s_addr;
};
struct sockaddr_in {
  short sin_family;
  unsigned short sin_port;
  struct in_addr sin_addr;
  char sin_zero[8];
};
struct ip_mreq {
  struct in_addr imr_multiaddr;
  struct in_addr imr_interface;
};
using ssize_t = intptr_t;
inline int socket(int, int, int) { return -1; }
inline int setsockopt(int, int, int, const void *, size_t) { return -1; }
inline int bind(int, const struct sockaddr *, size_t) { return -1; }
inline int close(int) { return -1; }
inline int inet_pton(int, const char *, void *) { return -1; }
inline uint16_t htons(uint16_t x) { return x; }
inline ssize_t recvfrom(int, void *, size_t, int, void *, void *) { return -1; }
struct cpu_set_t {
  unsigned long __bits[16];
};
#define CPU_ZERO(s) memset(s, 0, sizeof(cpu_set_t))
#define CPU_SET(c, s) ((void)0)
inline int pthread_setaffinity_np(void *, size_t, cpu_set_t *) { return -1; }
#else
// POSIX networking headers (Linux/WSL)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#endif

namespace hft {
namespace core {

// Packet data structure for queue storage
struct alignas(config::CACHELINE_SIZE) Packet {
  alignas(16) uint8_t data[config::MAX_PACKET_SIZE];
  uint32_t length;
  Timestamp timestamp;

  Packet() noexcept : length(0), timestamp(0) {}
};

// UDP receiver configuration
struct UDPConfig {
  std::string interface_ip{"0.0.0.0"};
  std::string multicast_group{"239.1.1.1"};
  uint16_t port{10000};
  size_t buffer_size{config::MAX_PACKET_SIZE * 1024};
  bool enable_timestamps{true};

  UDPConfig() = default;
};

// UDP receiver - captures market data packets using lock-free queue
class UDPReceiver {
public:
  explicit UDPReceiver(const UDPConfig &config = UDPConfig{})
      : config_(config), running_(false), socket_fd_(-1), stats_() {}

  ~UDPReceiver() { stop(); }

  // Initialize socket and join multicast group
  bool initialize() {
    // Create UDP socket
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
      return false;
    }

    // Set socket options for performance
    int reuse = 1;
    setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Increase receive buffer
    int bufsize = static_cast<int>(config_.buffer_size);
    setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

    // Enable timestamps if requested
    if (config_.enable_timestamps) {
      int enable = 1;
      setsockopt(socket_fd_, SOL_SOCKET, SO_TIMESTAMP, &enable, sizeof(enable));
    }

    // Bind to port
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(socket_fd_, reinterpret_cast<struct sockaddr *>(&addr),
             sizeof(addr)) < 0) {
      close(socket_fd_);
      socket_fd_ = -1;
      return false;
    }

    // Join multicast group
    struct ip_mreq mreq{};
    inet_pton(AF_INET, config_.multicast_group.c_str(), &mreq.imr_multiaddr);
    inet_pton(AF_INET, config_.interface_ip.c_str(), &mreq.imr_interface);

    if (setsockopt(socket_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                   sizeof(mreq)) < 0) {
      close(socket_fd_);
      socket_fd_ = -1;
      return false;
    }

    return true;
  }

  // Start receiving thread
  void start(int cpu_affinity = -1) {
    if (running_.load() || socket_fd_ < 0)
      return;

    running_.store(true);
    receive_thread_ = std::thread(&UDPReceiver::receive_loop, this);

    // Set CPU affinity
    if (cpu_affinity >= 0) {
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      CPU_SET(cpu_affinity, &cpuset);
      pthread_setaffinity_np(receive_thread_.native_handle(), sizeof(cpu_set_t),
                             &cpuset);
    }
  }

  // Stop receiving
  void stop() {
    if (!running_.load())
      return;

    running_.store(false);
    if (receive_thread_.joinable()) {
      receive_thread_.join();
    }

    if (socket_fd_ >= 0) {
      close(socket_fd_);
      socket_fd_ = -1;
    }
  }

  // Read next packet from queue into a MessageView
  // Note: The view is valid until the next call to read_packet
  bool read_packet(MessageView &view) noexcept {
    if (!packet_queue_.pop(current_packet_)) {
      return false;
    }

    view.data = current_packet_.data;
    view.length = current_packet_.length;
    view.timestamp = current_packet_.timestamp;
    view.sequence = sequence_++;

    return true;
  }

  // Check if packets are available
  bool has_packets() const noexcept { return !packet_queue_.empty(); }

  // Get statistics
  const Statistics &get_stats() const noexcept { return stats_; }

private:
  void receive_loop() {
    Packet packet;

    while (running_.load(std::memory_order_relaxed)) {
      ssize_t received = recvfrom(socket_fd_, packet.data,
                                  config::MAX_PACKET_SIZE, 0, nullptr, nullptr);

      if (received > 0) {
        packet.length = static_cast<uint32_t>(received);
        packet.timestamp = get_timestamp();

        if (packet_queue_.push(packet)) {
          stats_.packets_received++;
        } else {
          stats_.packets_dropped++;
        }
      }
    }
  }

  UDPConfig config_;
  std::atomic<bool> running_;
  int socket_fd_;
  SPSCQueue<Packet, config::PACKET_RING_SIZE> packet_queue_;
  Packet current_packet_; // Holds the last popped packet
  uint32_t sequence_{0};  // Running sequence number
  std::thread receive_thread_;
  Statistics stats_;
};

} // namespace core
} // namespace hft
