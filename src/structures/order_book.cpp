#include "structures/order_book.hpp"
#include <algorithm>
#include <cstring>

namespace hft {

size_t OrderBook::find_bid_insertion_point(Price price) const noexcept {
    // Binary search for insertion point (bids are sorted descending)
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

size_t OrderBook::find_ask_insertion_point(Price price) const noexcept {
    // Binary search for insertion point (asks are sorted ascending)
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

void OrderBook::insert_bid_level(size_t index, Price price) noexcept {
    if (bid_count_ >= MAX_PRICE_LEVELS) {
        return; // No space
    }
    
    // Shift elements to make room
    if (index < bid_count_) {
        std::memmove(&bid_levels_[index + 1], &bid_levels_[index], 
                    (bid_count_ - index) * sizeof(PriceLevel));
        
        // Update price-to-level mapping
        for (size_t i = index + 1; i <= bid_count_; ++i) {
            price_to_level_bid_[bid_levels_[i].price] = i;
        }
    }
    
    // Initialize new level
    bid_levels_[index] = PriceLevel();
    bid_levels_[index].price = price;
    price_to_level_bid_[price] = index;
    ++bid_count_;
}

void OrderBook::insert_ask_level(size_t index, Price price) noexcept {
    if (ask_count_ >= MAX_PRICE_LEVELS) {
        return; // No space
    }
    
    // Shift elements to make room
    if (index < ask_count_) {
        std::memmove(&ask_levels_[index + 1], &ask_levels_[index], 
                    (ask_count_ - index) * sizeof(PriceLevel));
        
        // Update price-to-level mapping
        for (size_t i = index + 1; i <= ask_count_; ++i) {
            price_to_level_ask_[ask_levels_[i].price] = i;
        }
    }
    
    // Initialize new level
    ask_levels_[index] = PriceLevel();
    ask_levels_[index].price = price;
    price_to_level_ask_[price] = index;
    ++ask_count_;
}

void OrderBook::remove_bid_level(size_t index) noexcept {
    if (index >= bid_count_) {
        return;
    }
    
    Price price = bid_levels_[index].price;
    price_to_level_bid_.erase(price);
    
    // Shift elements
    if (index < bid_count_ - 1) {
        std::memmove(&bid_levels_[index], &bid_levels_[index + 1], 
                    (bid_count_ - index - 1) * sizeof(PriceLevel));
        
        // Update price-to-level mapping
        for (size_t i = index; i < bid_count_ - 1; ++i) {
            price_to_level_bid_[bid_levels_[i].price] = i;
        }
    }
    
    --bid_count_;
}

void OrderBook::remove_ask_level(size_t index) noexcept {
    if (index >= ask_count_) {
        return;
    }
    
    Price price = ask_levels_[index].price;
    price_to_level_ask_.erase(price);
    
    // Shift elements
    if (index < ask_count_ - 1) {
        std::memmove(&ask_levels_[index], &ask_levels_[index + 1], 
                    (ask_count_ - index - 1) * sizeof(PriceLevel));
        
        // Update price-to-level mapping
        for (size_t i = index; i < ask_count_ - 1; ++i) {
            price_to_level_ask_[ask_levels_[i].price] = i;
        }
    }
    
    --ask_count_;
}

bool OrderBook::add_order(OrderId id, Price price, Quantity quantity, Side side) noexcept {
    // Check if order already exists
    if (orders_.find(id) != orders_.end()) {
        return false;
    }
    
    // Allocate order from pool
    Order* order = order_pool_->construct(id, price, quantity, side);
    if (!order) {
        return false; // Pool exhausted
    }
    
    orders_[id] = order;
    
    if (side == Side::BUY) {
        // Handle bid side
        auto it = price_to_level_bid_.find(price);
        size_t level_index;
        
        if (it == price_to_level_bid_.end()) {
            // Create new price level
            level_index = find_bid_insertion_point(price);
            insert_bid_level(level_index, price);
        } else {
            level_index = it->second;
        }
        
        PriceLevel& level = bid_levels_[level_index];
        
        // Add order to level
        if (level.first_order == nullptr) {
            level.first_order = level.last_order = order;
        } else {
            level.last_order = order;
        }
        
        level.total_quantity += quantity;
        ++level.order_count;
        
    } else {
        // Handle ask side
        auto it = price_to_level_ask_.find(price);
        size_t level_index;
        
        if (it == price_to_level_ask_.end()) {
            // Create new price level
            level_index = find_ask_insertion_point(price);
            insert_ask_level(level_index, price);
        } else {
            level_index = it->second;
        }
        
        PriceLevel& level = ask_levels_[level_index];
        
        // Add order to level
        if (level.first_order == nullptr) {
            level.first_order = level.last_order = order;
        } else {
            level.last_order = order;
        }
        
        level.total_quantity += quantity;
        ++level.order_count;
    }
    
    return true;
}

bool OrderBook::modify_order(OrderId id, Quantity new_quantity) noexcept {
    auto it = orders_.find(id);
    if (it == orders_.end()) {
        return false;
    }
    
    Order* order = it->second;
    Quantity old_quantity = order->quantity;
    order->quantity = new_quantity;
    
    // Update price level quantities
    if (order->side == Side::BUY) {
        auto level_it = price_to_level_bid_.find(order->price);
        if (level_it != price_to_level_bid_.end()) {
            PriceLevel& level = bid_levels_[level_it->second];
            level.total_quantity = level.total_quantity - old_quantity + new_quantity;
        }
    } else {
        auto level_it = price_to_level_ask_.find(order->price);
        if (level_it != price_to_level_ask_.end()) {
            PriceLevel& level = ask_levels_[level_it->second];
            level.total_quantity = level.total_quantity - old_quantity + new_quantity;
        }
    }
    
    return true;
}

bool OrderBook::cancel_order(OrderId id) noexcept {
    auto it = orders_.find(id);
    if (it == orders_.end()) {
        return false;
    }
    
    Order* order = it->second;
    Price price = order->price;
    Quantity quantity = order->quantity;
    Side side = order->side;
    
    // Remove from orders map
    orders_.erase(it);
    
    if (side == Side::BUY) {
        auto level_it = price_to_level_bid_.find(price);
        if (level_it != price_to_level_bid_.end()) {
            size_t level_index = level_it->second;
            PriceLevel& level = bid_levels_[level_index];
            
            level.total_quantity -= quantity;
            --level.order_count;
            
            // Remove price level if empty
            if (level.order_count == 0) {
                remove_bid_level(level_index);
            }
        }
    } else {
        auto level_it = price_to_level_ask_.find(price);
        if (level_it != price_to_level_ask_.end()) {
            size_t level_index = level_it->second;
            PriceLevel& level = ask_levels_[level_index];
            
            level.total_quantity -= quantity;
            --level.order_count;
            
            // Remove price level if empty
            if (level.order_count == 0) {
                remove_ask_level(level_index);
            }
        }
    }
    
    // Return order to pool
    order_pool_->destroy(order);
    
    return true;
}

void OrderBook::clear() noexcept {
    // Return all orders to pool
    for (auto& pair : orders_) {
        order_pool_->destroy(pair.second);
    }
    
    orders_.clear();
    price_to_level_bid_.clear();
    price_to_level_ask_.clear();
    
    bid_count_ = 0;
    ask_count_ = 0;
}

} // namespace hft
