#pragma once

#include "../../core/parser/parser_interface.hpp"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>


namespace hft {
namespace protocols {

// Parser factory function type
using ParserFactory = std::function<std::unique_ptr<core::IParser>()>;

// Parser registry - singleton pattern
class ParserRegistry {
public:
  static ParserRegistry &instance() {
    static ParserRegistry registry;
    return registry;
  }

  // Register a parser factory
  bool register_parser(const std::string &name, ParserFactory factory) {
    auto [it, inserted] = factories_.emplace(name, std::move(factory));
    return inserted;
  }

  // Create a parser by name
  std::unique_ptr<core::IParser> create_parser(const std::string &name) const {
    auto it = factories_.find(name);
    if (it != factories_.end()) {
      return it->second();
    }
    return nullptr;
  }

  // Get list of registered parsers
  std::vector<std::string> list_parsers() const {
    std::vector<std::string> names;
    names.reserve(factories_.size());
    for (const auto &[name, _] : factories_) {
      names.push_back(name);
    }
    return names;
  }

  // Check if parser is registered
  bool has_parser(const std::string &name) const {
    return factories_.find(name) != factories_.end();
  }

private:
  ParserRegistry() = default;
  ParserRegistry(const ParserRegistry &) = delete;
  ParserRegistry &operator=(const ParserRegistry &) = delete;

  std::unordered_map<std::string, ParserFactory> factories_;
};

// Helper class for automatic reigstration
template <typename ParserType> class ParserRegistration {
public:
  explicit ParserRegistration(const std::string &name) {
    ParserRegistry::instance().register_parser(
        name, []() { return std::make_unique<ParserType>(); });
  }
};

// Macro for easy parser registration
#define REGISTER_PARSER(ParserClass, name)                                     \
  static hft::protocols::ParserRegistration<ParserClass>                       \
      __parser_registration_##ParserClass(name);
} // namespace protocols

} // namespace hft