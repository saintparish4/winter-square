#pragma once

#include "common/types.hpp"
#include "memory/object_pool.hpp"
#include <array>
#include <unordered_map>

namespace hft {

struct Order {
    OrderId id;
    Price price;
    Quantity quantity;
    Side side;
    Timestamp timestamp;
    
    Order() = default;
    Order(OrderId id_, Price price_, Quantity qty_, Side side_)
        : id(id_), price(price_), quantity(qty_), side(side_)
        , timestamp(std::chrono::high_resolution_clock::now()) {}
};

struct PriceLevel {
    Price price;
    Quantity total_quantity;
    uint32_t order_count;
    Order* first_order;
    Order* last_order;
    
    PriceLevel() : price(0), total_quantity(0), order_count(0)
                 , first_order(nullptr), last_order(nullptr) {}
};

class OrderBook {
private:
    static constexpr size_t MAX_PRICE_LEVELS = 1000;
    static constexpr size_t MAX_ORDERS_PER_LEVEL = 100;
    
    // Pre-allocated arrays for ultra-fast access
    CACHE_ALIGNED std::array<PriceLevel, MAX_PRICE_LEVELS> bid_levels_;
    CACHE_ALIGNED std::array<PriceLevel, MAX_PRICE_LEVELS> ask_levels_;
    
    // Hash maps for O(1) order lookup
    std::unordered_map<OrderId, Order*> orders_;
    std::unordered_map<Price, size_t> price_to_level_bid_;
    std::unordered_map<Price, size_t> price_to_level_ask_;
    
    size_t bid_count_ = 0;
    size_t ask_count_ = 0;
    
    ObjectPool<Order>* order_pool_;
    
    FORCE_INLINE size_t find_bid_insertion_point(Price price) const noexcept;
    FORCE_INLINE size_t find_ask_insertion_point(Price price) const noexcept;
    FORCE_INLINE void insert_bid_level(size_t index, Price price) noexcept;
    FORCE_INLINE void insert_ask_level(size_t index, Price price) noexcept;
    FORCE_INLINE void remove_bid_level(size_t index) noexcept;
    FORCE_INLINE void remove_ask_level(size_t index) noexcept;

public:
    explicit OrderBook(ObjectPool<Order>* pool) : order_pool_(pool) {
        orders_.reserve(10000);
        price_to_level_bid_.reserve(MAX_PRICE_LEVELS);
        price_to_level_ask_.reserve(MAX_PRICE_LEVELS);
    }
    
    // Core order book operations
    FORCE_INLINE bool add_order(OrderId id, Price price, Quantity quantity, Side side) noexcept;
    FORCE_INLINE bool modify_order(OrderId id, Quantity new_quantity) noexcept;
    FORCE_INLINE bool cancel_order(OrderId id) noexcept;
    
    // Market data access
    FORCE_INLINE const PriceLevel* get_best_bid() const noexcept {
        return bid_count_ > 0 ? &bid_levels_[0] : nullptr;
    }
    
    FORCE_INLINE const PriceLevel* get_best_ask() const noexcept {
        return ask_count_ > 0 ? &ask_levels_[0] : nullptr;
    }
    
    FORCE_INLINE Price get_mid_price() const noexcept {
        const auto* best_bid = get_best_bid();
        const auto* best_ask = get_best_ask();
        
        if (LIKELY(best_bid && best_ask)) {
            return (best_bid->price + best_ask->price) / 2;
        }
        return 0;
    }
    
    FORCE_INLINE Price get_spread() const noexcept {
        const auto* best_bid = get_best_bid();
        const auto* best_ask = get_best_ask();
        
        if (LIKELY(best_bid && best_ask)) {
            return best_ask->price - best_bid->price;
        }
        return 0;
    }
    
    // Level access for market depth
    FORCE_INLINE const PriceLevel* get_bid_level(size_t level) const noexcept {
        return level < bid_count_ ? &bid_levels_[level] : nullptr;
    }
    
    FORCE_INLINE const PriceLevel* get_ask_level(size_t level) const noexcept {
        return level < ask_count_ ? &ask_levels_[level] : nullptr;
    }
    
    FORCE_INLINE size_t get_bid_depth() const noexcept { return bid_count_; }
    FORCE_INLINE size_t get_ask_depth() const noexcept { return ask_count_; }
    
    // Statistics
    FORCE_INLINE size_t get_total_orders() const noexcept { return orders_.size(); }
    
    void clear() noexcept;
};

} // namespace hft
