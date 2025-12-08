#pragma once

#include "../types.hpp"
#include <memory>
#include <string>

namespace hft::core {

// Parser interface - implement for each market data protocol
// Examples: ITCH 5.0, PITCH, SBE, FIX
class IParser {
public:
    virtual ~IParser() = default;
    
    // Parse a raw packet into normalized messages
    // Returns number of messages parsed (can be 0 or multiple)
    // Parsed messages are written to 'output' array
    // max_messages specifies the size of output array
    virtual size_t parse(
        const MessageView& raw_packet,
        NormalizedMessage* output,
        size_t max_messages
    ) noexcept = 0;
    
    // Get parser name
    virtual const char* name() const noexcept = 0;
    
    // Optional: Initialize parser state
    virtual void initialize() {}
    
    // Optional: Reset parser state
    virtual void reset() {}
    
    // Optional: Get parser statistics
    virtual void get_stats(Statistics& stats) const {
        (void)stats;
    }
};

// Null parser - passes through without parsing (for testing)
class NullParser : public IParser {
public:
    size_t parse(
        const MessageView& raw_packet,
        NormalizedMessage* output,
        size_t max_messages
    ) noexcept override {
        if (max_messages == 0) return 0;
        
        // Create a dummy normalized message
        output[0] = NormalizedMessage{};
        output[0].type = NormalizedMessage::Type::UNKNOWN;
        output[0].local_timestamp = raw_packet.timestamp;
        output[0].sequence = raw_packet.sequence;
        
        return 1;
    }
    
    const char* name() const noexcept override {
        return "NullParser";
    }
};

// Echo parser - for testing, just echoes packet info
class EchoParser : public IParser {
public:
    size_t parse(
        const MessageView& raw_packet,
        NormalizedMessage* output,
        size_t max_messages
    ) noexcept override {
        if (max_messages == 0 || !raw_packet.is_valid()) return 0;
        
        // Create message with packet metadata
        output[0] = NormalizedMessage{};
        output[0].type = NormalizedMessage::Type::SYSTEM_EVENT;
        output[0].local_timestamp = raw_packet.timestamp;
        output[0].sequence = raw_packet.sequence;
        output[0].quantity = raw_packet.length;  // Store length in quantity field
        
        parse_count_++;
        return 1;
    }
    
    const char* name() const noexcept override {
        return "EchoParser";
    }
    
    void get_stats(Statistics& stats) const override {
        stats.messages_parsed = parse_count_;
    }
    
private:
    mutable uint64_t parse_count_{0};
};

// Parser factory type
using ParserFactory = std::unique_ptr<IParser>(*)();

// Register a parser (for plugin system)
struct ParserRegistration {
    const char* name;
    ParserFactory factory;
};

} // namespace hft::core