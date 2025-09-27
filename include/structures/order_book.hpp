#pragma once

#include "common/types.hpp"
#include "memory/object_pool.hpp"
#include <array>
#include <unordered_map>
#include <cstring>

namespace hft {

struct Order {
    OrderId id;
    Price price;
    Quantity quantity;
    Side side;
    Timestamp timestamp;
    
    // Linked list pointers for orders at same price level
    Order* next_order;
    Order* prev_order;
    
    Order() = default;
    Order(OrderId id_, Price price_, Quantity qty_, Side side_)
        : id(id_), price(price_), quantity(qty_), side(side_)
        , timestamp(std::chrono::steady_clock::now())
        , next_order(nullptr), prev_order(nullptr) {}
};

struct PriceLevel {
    Price price;
    Quantity total_quantity;
    uint32_t order_count;
    Order* first_order;
    Order* last_order;
    
    PriceLevel() : price(0), total_quantity(0), order_count(0)
                 , first_order(nullptr), last_order(nullptr) {}
    
    // Add order to end of price level (FIFO)
    FORCE_INLINE void add_order(Order* order) noexcept {
        order->next_order = nullptr;
        order->prev_order = last_order;
        
        if (LIKELY(last_order)) {
            last_order->next_order = order;
        } else {
            first_order = order;
        }
        
        last_order = order;
        total_quantity += order->quantity;
        ++order_count;
    }
    
    // Remove order from price level
    FORCE_INLINE void remove_order(Order* order) noexcept {
        if (order->prev_order) {
            order->prev_order->next_order = order->next_order;
        } else {
            first_order = order->next_order;
        }
        
        if (order->next_order) {
            order->next_order->prev_order = order->prev_order;
        } else {
            last_order = order->prev_order;
        }
        
        total_quantity -= order->quantity;
        --order_count;
    }
    
    // Update order quantity
    FORCE_INLINE void update_quantity(Order* order, Quantity old_qty, Quantity new_qty) noexcept {
        total_quantity = total_quantity - old_qty + new_qty;
    }
    
    FORCE_INLINE bool empty() const noexcept {
        return order_count == 0;
    }
};

class OrderBook {
private:
    static constexpr size_t MAX_PRICE_LEVELS = 1000;
    static constexpr size_t INITIAL_ORDER_RESERVE = 10000;
    static constexpr size_t HASH_MAP_LOAD_FACTOR_PERCENT = 75;
    
    // Pre-allocated arrays for ultra-fast access
    CACHE_ALIGNED std::array<PriceLevel, MAX_PRICE_LEVELS> bid_levels_;
    CACHE_ALIGNED std::array<PriceLevel, MAX_PRICE_LEVELS> ask_levels_;
    
    // Hash maps for O(1) order lookup - using reserve to avoid rehashing
    std::unordered_map<OrderId, Order*> orders_;
    std::unordered_map<Price, size_t> price_to_level_bid_;
    std::unordered_map<Price, size_t> price_to_level_ask_;
    
    // Cache-aligned counters
    CACHE_ALIGNED size_t bid_count_ = 0;
    CACHE_ALIGNED size_t ask_count_ = 0;
    
    ObjectPool<Order>* order_pool_;
    
    // Statistics for monitoring
    CACHE_ALIGNED mutable size_t stats_adds_ = 0;
    CACHE_ALIGNED mutable size_t stats_modifies_ = 0;
    CACHE_ALIGNED mutable size_t stats_cancels_ = 0;
    
    // Internal helper methods
    FORCE_INLINE size_t find_bid_insertion_point(Price price) const noexcept {
        // Binary search for insertion point (descending order for bids)
        size_t left = 0, right = bid_count_;
        while (left < right) {
            size_t mid = (left + right) / 2;
            if (bid_levels_[mid].price > price) {
                left = mid + 1;
            } else {
                right = mid;
            }
        }
        return left;
    }
    
    FORCE_INLINE size_t find_ask_insertion_point(Price price) const noexcept {
        // Binary search for insertion point (ascending order for asks)
        size_t left = 0, right = ask_count_;
        while (left < right) {
            size_t mid = (left + right) / 2;
            if (ask_levels_[mid].price < price) {
                left = mid + 1;
            } else {
                right = mid;
            }
        }
        return left;
    }
    
    FORCE_INLINE void insert_bid_level(size_t index, Price price) noexcept {
        if (UNLIKELY(bid_count_ >= MAX_PRICE_LEVELS)) return;
        
        // Shift levels to make room
        if (index < bid_count_) {
            std::memmove(&bid_levels_[index + 1], &bid_levels_[index], 
                        (bid_count_ - index) * sizeof(PriceLevel));
        }
        
        // Initialize new level
        bid_levels_[index] = PriceLevel();
        bid_levels_[index].price = price;
        
        // Update hash map
        price_to_level_bid_[price] = index;
        
        // Update indices in hash map for shifted levels
        for (size_t i = index + 1; i <= bid_count_; ++i) {
            price_to_level_bid_[bid_levels_[i].price] = i;
        }
        
        ++bid_count_;
    }
    
    FORCE_INLINE void insert_ask_level(size_t index, Price price) noexcept {
        if (UNLIKELY(ask_count_ >= MAX_PRICE_LEVELS)) return;
        
        // Shift levels to make room
        if (index < ask_count_) {
            std::memmove(&ask_levels_[index + 1], &ask_levels_[index], 
                        (ask_count_ - index) * sizeof(PriceLevel));
        }
        
        // Initialize new level
        ask_levels_[index] = PriceLevel();
        ask_levels_[index].price = price;
        
        // Update hash map
        price_to_level_ask_[price] = index;
        
        // Update indices in hash map for shifted levels
        for (size_t i = index + 1; i <= ask_count_; ++i) {
            price_to_level_ask_[ask_levels_[i].price] = i;
        }
        
        ++ask_count_;
    }
    
    FORCE_INLINE void remove_bid_level(size_t index) noexcept {
        if (UNLIKELY(index >= bid_count_)) return;
        
        Price price = bid_levels_[index].price;
        
        // Remove from hash map
        price_to_level_bid_.erase(price);
        
        // Shift levels down
        if (index < bid_count_ - 1) {
            std::memmove(&bid_levels_[index], &bid_levels_[index + 1], 
                        (bid_count_ - index - 1) * sizeof(PriceLevel));
        }
        
        --bid_count_;
        
        // Update indices in hash map for shifted levels
        for (size_t i = index; i < bid_count_; ++i) {
            price_to_level_bid_[bid_levels_[i].price] = i;
        }
    }
    
    FORCE_INLINE void remove_ask_level(size_t index) noexcept {
        if (UNLIKELY(index >= ask_count_)) return;
        
        Price price = ask_levels_[index].price;
        
        // Remove from hash map
        price_to_level_ask_.erase(price);
        
        // Shift levels down
        if (index < ask_count_ - 1) {
            std::memmove(&ask_levels_[index], &ask_levels_[index + 1], 
                        (ask_count_ - index - 1) * sizeof(PriceLevel));
        }
        
        --ask_count_;
        
        // Update indices in hash map for shifted levels
        for (size_t i = index; i < ask_count_; ++i) {
            price_to_level_ask_[ask_levels_[i].price] = i;
        }
    }

public:
    explicit OrderBook(ObjectPool<Order>* pool) : order_pool_(pool) {
        // Pre-allocate hash maps to avoid rehashing during operation
        orders_.reserve(INITIAL_ORDER_RESERVE);
        price_to_level_bid_.reserve(MAX_PRICE_LEVELS);
        price_to_level_ask_.reserve(MAX_PRICE_LEVELS);
        
        // Set load factor to avoid rehashing
        orders_.max_load_factor(HASH_MAP_LOAD_FACTOR_PERCENT / 100.0f);
        price_to_level_bid_.max_load_factor(HASH_MAP_LOAD_FACTOR_PERCENT / 100.0f);
        price_to_level_ask_.max_load_factor(HASH_MAP_LOAD_FACTOR_PERCENT / 100.0f);
    }
    
    // Non-copyable, non-movable for performance
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;
    
    // Core order book operations
    FORCE_INLINE HOT_PATH bool add_order(OrderId id, Price price, Quantity quantity, Side side) noexcept {
        if (UNLIKELY(!order_pool_ || quantity == 0 || price == 0)) {
            return false;
        }
        
        // Check if order already exists
        if (UNLIKELY(orders_.find(id) != orders_.end())) {
            return false;
        }
        
        // Allocate order from pool
        Order* order = order_pool_->construct(id, price, quantity, side);
        if (UNLIKELY(!order)) {
            return false;
        }
        
        // Add to order lookup
        orders_[id] = order;
        
        if (side == Side::BUY) {
            auto it = price_to_level_bid_.find(price);
            if (it != price_to_level_bid_.end()) {
                // Price level exists
                bid_levels_[it->second].add_order(order);
            } else {
                // Create new price level
                size_t index = find_bid_insertion_point(price);
                insert_bid_level(index, price);
                bid_levels_[index].add_order(order);
            }
        } else {
            auto it = price_to_level_ask_.find(price);
            if (it != price_to_level_ask_.end()) {
                // Price level exists
                ask_levels_[it->second].add_order(order);
            } else {
                // Create new price level
                size_t index = find_ask_insertion_point(price);
                insert_ask_level(index, price);
                ask_levels_[index].add_order(order);
            }
        }
        
        ++stats_adds_;
        return true;
    }
    
    FORCE_INLINE HOT_PATH bool modify_order(OrderId id, Quantity new_quantity) noexcept {
        if (UNLIKELY(new_quantity == 0)) {
            return cancel_order(id);
        }
        
        auto it = orders_.find(id);
        if (UNLIKELY(it == orders_.end())) {
            return false;
        }
        
        Order* order = it->second;
        Quantity old_quantity = order->quantity;
        
        if (old_quantity == new_quantity) {
            return true; // No change needed
        }
        
        // Find the price level
        if (order->side == Side::BUY) {
            auto level_it = price_to_level_bid_.find(order->price);
            if (LIKELY(level_it != price_to_level_bid_.end())) {
                bid_levels_[level_it->second].update_quantity(order, old_quantity, new_quantity);
            }
        } else {
            auto level_it = price_to_level_ask_.find(order->price);
            if (LIKELY(level_it != price_to_level_ask_.end())) {
                ask_levels_[level_it->second].update_quantity(order, old_quantity, new_quantity);
            }
        }
        
        order->quantity = new_quantity;
        order->timestamp = std::chrono::steady_clock::now();
        
        ++stats_modifies_;
        return true;
    }
    
    FORCE_INLINE HOT_PATH bool cancel_order(OrderId id) noexcept {
        auto it = orders_.find(id);
        if (UNLIKELY(it == orders_.end())) {
            return false;
        }
        
        Order* order = it->second;
        
        // Find and update price level
        if (order->side == Side::BUY) {
            auto level_it = price_to_level_bid_.find(order->price);
            if (LIKELY(level_it != price_to_level_bid_.end())) {
                size_t level_index = level_it->second;
                bid_levels_[level_index].remove_order(order);
                
                // Remove price level if empty
                if (bid_levels_[level_index].empty()) {
                    remove_bid_level(level_index);
                }
            }
        } else {
            auto level_it = price_to_level_ask_.find(order->price);
            if (LIKELY(level_it != price_to_level_ask_.end())) {
                size_t level_index = level_it->second;
                ask_levels_[level_index].remove_order(order);
                
                // Remove price level if empty
                if (ask_levels_[level_index].empty()) {
                    remove_ask_level(level_index);
                }
            }
        }
        
        // Remove from order lookup and return to pool
        orders_.erase(it);
        order_pool_->destroy(order);
        
        ++stats_cancels_;
        return true;
    }
    
    // Market data access - optimized for frequent calls
    FORCE_INLINE PURE_FUNCTION const PriceLevel* get_best_bid() const noexcept {
        return LIKELY(bid_count_ > 0) ? &bid_levels_[0] : nullptr;
    }
    
    FORCE_INLINE PURE_FUNCTION const PriceLevel* get_best_ask() const noexcept {
        return LIKELY(ask_count_ > 0) ? &ask_levels_[0] : nullptr;
    }
    
    FORCE_INLINE PURE_FUNCTION Price get_mid_price() const noexcept {
        const auto* best_bid = get_best_bid();
        const auto* best_ask = get_best_ask();
        
        if (LIKELY(best_bid && best_ask)) {
            return (best_bid->price + best_ask->price) / 2;
        }
        return 0;
    }
    
    FORCE_INLINE PURE_FUNCTION Price get_spread() const noexcept {
        const auto* best_bid = get_best_bid();
        const auto* best_ask = get_best_ask();
        
        if (LIKELY(best_bid && best_ask)) {
            return best_ask->price - best_bid->price;
        }
        return Price(-1); // Invalid spread indicator
    }
    
    // Level access for market depth
    FORCE_INLINE PURE_FUNCTION const PriceLevel* get_bid_level(size_t level) const noexcept {
        return LIKELY(level < bid_count_) ? &bid_levels_[level] : nullptr;
    }
    
    FORCE_INLINE PURE_FUNCTION const PriceLevel* get_ask_level(size_t level) const noexcept {
        return LIKELY(level < ask_count_) ? &ask_levels_[level] : nullptr;
    }
    
    FORCE_INLINE PURE_FUNCTION size_t get_bid_depth() const noexcept { return bid_count_; }
    FORCE_INLINE PURE_FUNCTION size_t get_ask_depth() const noexcept { return ask_count_; }
    
    // Order lookup
    FORCE_INLINE const Order* find_order(OrderId id) const noexcept {
        auto it = orders_.find(id);
        return (it != orders_.end()) ? it->second : nullptr;
    }
    
    // Statistics and monitoring
    FORCE_INLINE PURE_FUNCTION size_t get_total_orders() const noexcept { 
        return orders_.size(); 
    }
    
    FORCE_INLINE PURE_FUNCTION size_t get_total_bid_quantity() const noexcept {
        Quantity total = 0;
        for (size_t i = 0; i < bid_count_; ++i) {
            total += bid_levels_[i].total_quantity;
        }
        return total;
    }
    
    FORCE_INLINE PURE_FUNCTION size_t get_total_ask_quantity() const noexcept {
        Quantity total = 0;
        for (size_t i = 0; i < ask_count_; ++i) {
            total += ask_levels_[i].total_quantity;
        }
        return total;
    }
    
    // Performance statistics
    struct Statistics {
        size_t total_adds;
        size_t total_modifies;
        size_t total_cancels;
        size_t current_orders;
        size_t bid_levels;
        size_t ask_levels;
    };
    
    FORCE_INLINE Statistics get_statistics() const noexcept {
        return {
            stats_adds_,
            stats_modifies_, 
            stats_cancels_,
            orders_.size(),
            bid_count_,
            ask_count_
        };
    }
    
    void clear() noexcept {
        // Clear all orders from pool
        for (auto& [id, order] : orders_) {
            order_pool_->destroy(order);
        }
        
        orders_.clear();
        price_to_level_bid_.clear();
        price_to_level_ask_.clear();
        
        bid_count_ = 0;
        ask_count_ = 0;
        
        // Reset statistics
        stats_adds_ = 0;
        stats_modifies_ = 0;
        stats_cancels_ = 0;
    }
    
    // Validate internal consistency (debug/testing only)
    bool validate() const noexcept {
        // Check bid ordering (descending)
        for (size_t i = 1; i < bid_count_; ++i) {
            if (bid_levels_[i-1].price <= bid_levels_[i].price) {
                return false;
            }
        }
        
        // Check ask ordering (ascending)
        for (size_t i = 1; i < ask_count_; ++i) {
            if (ask_levels_[i-1].price >= ask_levels_[i].price) {
                return false;
            }
        }
        
        return true;
    }
};

} // namespace hft