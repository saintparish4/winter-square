#pragma once

#include "../types.hpp"
#include <memory>

namespace hft {
namespace core {
// Pure virtual interface for message subscribers
// Plugins implement this to receive market data
class ISubscriber {
public:
  virtual ~ISubscriber() = default;

  // Called when a new message is available
  // Must be thread-safe and non-blocking
  // Return false to unsubscribe
  virtual bool on_message(const NormalizedMessage &msg) noexcept = 0;

  // Optional: Called on raw packet before parsing
  // Allows subscribers to access raw data if needed
  virtual void on_raw_packet(const MessageView &view) noexcept {
    (void)view; // Default: ignore raw packets
  }

  // Get subscriber name for logging/debugging
  virtual const char *name() const noexcept = 0;

  // Optional: Initialize subscriber
  virtual void initialize() {}

  // Optional: Cleanup on shutdown
  virtual void shutdown() {}
};

// Callback-based subscriber for simple use cases
class CallbackSubscriber : public ISubscriber {
public:
  using Callback = bool (*)(const NormalizedMessage &, void *user_data);

  CallbackSubscriber(const char *name, Callback cb, void *user_data = nullptr)
      : name_(name), callback_(cb), user_data_(user_data) {}

  bool on_message(const NormalizedMessage &msg) noexcept override {
    return callback_(msg, user_data_);
  }

  const char *name() const noexcept override { return name_; }

private:
  const char *name_;
  Callback callback_;
  void *user_data_;
};

// Lambda-based subscriber for C++ convenience
template <typename F> class LambdaSubscriber : public ISubscriber {
public:
  LambdaSubscriber(const char *name, F &&func)
      : name_(name), func_(std::forward<F>(func)) {}

  bool on_message(const NormalizedMessage &msg) noexcept override {
    return func_(msg);
  }

  const char *name() const noexcept override { return name_; }

private:
  const char *name_;
  F func_;
};

// Helper to create lambda subscriber
template <typename F> auto make_subscriber(const char *name, F &&func) {
  return std::make_unique<LambdaSubscriber<F>>(name, std::forward<F>(func));
}

} // namespace core
} // namespace hft