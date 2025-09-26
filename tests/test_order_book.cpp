#include "structures/order_book.hpp"
#include "memory/object_pool.hpp"
#include <cassert>
#include <iostream>

using namespace hft;

void test_basic_order_operations() {
    std::cout << "Testing basic order operations..." << std::endl;
    
    ObjectPool<Order> order_pool;
    OrderBook book(&order_pool);
    
    // Test empty book
    assert(book.get_best_bid() == nullptr);
    assert(book.get_best_ask() == nullptr);
    assert(book.get_mid_price() == 0);
    assert(book.get_spread() == 0);
    assert(book.get_total_orders() == 0);
    
    // Add buy order
    assert(book.add_order(1, 10000, 1000, Side::BUY));
    assert(book.get_total_orders() == 1);
    
    const PriceLevel* best_bid = book.get_best_bid();
    assert(best_bid != nullptr);
    assert(best_bid->price == 10000);
    assert(best_bid->total_quantity == 1000);
    assert(best_bid->order_count == 1);
    
    // Add sell order
    assert(book.add_order(2, 10100, 500, Side::SELL));
    assert(book.get_total_orders() == 2);
    
    const PriceLevel* best_ask = book.get_best_ask();
    assert(best_ask != nullptr);
    assert(best_ask->price == 10100);
    assert(best_ask->total_quantity == 500);
    assert(best_ask->order_count == 1);
    
    // Test mid price and spread
    assert(book.get_mid_price() == 10050);
    assert(book.get_spread() == 100);
    
    std::cout << "Basic order operations: PASSED" << std::endl;
}

void test_price_level_ordering() {
    std::cout << "Testing price level ordering..." << std::endl;
    
    ObjectPool<Order> order_pool;
    OrderBook book(&order_pool);
    
    // Add buy orders (should be sorted descending)
    assert(book.add_order(1, 10000, 100, Side::BUY));
    assert(book.add_order(2, 10200, 200, Side::BUY));
    assert(book.add_order(3, 10100, 150, Side::BUY));
    
    // Best bid should be highest price
    const PriceLevel* best_bid = book.get_best_bid();
    assert(best_bid->price == 10200);
    
    // Check bid levels ordering
    assert(book.get_bid_level(0)->price == 10200); // Highest
    assert(book.get_bid_level(1)->price == 10100);
    assert(book.get_bid_level(2)->price == 10000); // Lowest
    assert(book.get_bid_depth() == 3);
    
    // Add sell orders (should be sorted ascending)
    assert(book.add_order(4, 10400, 100, Side::SELL));
    assert(book.add_order(5, 10300, 200, Side::SELL));
    assert(book.add_order(6, 10350, 150, Side::SELL));
    
    // Best ask should be lowest price
    const PriceLevel* best_ask = book.get_best_ask();
    assert(best_ask->price == 10300);
    
    // Check ask levels ordering
    assert(book.get_ask_level(0)->price == 10300); // Lowest
    assert(book.get_ask_level(1)->price == 10350);
    assert(book.get_ask_level(2)->price == 10400); // Highest
    assert(book.get_ask_depth() == 3);
    
    std::cout << "Price level ordering: PASSED" << std::endl;
}

void test_order_modifications() {
    std::cout << "Testing order modifications..." << std::endl;
    
    ObjectPool<Order> order_pool;
    OrderBook book(&order_pool);
    
    // Add orders
    assert(book.add_order(1, 10000, 1000, Side::BUY));
    assert(book.add_order(2, 10000, 500, Side::BUY));
    
    const PriceLevel* level = book.get_best_bid();
    assert(level->price == 10000);
    assert(level->total_quantity == 1500);
    assert(level->order_count == 2);
    
    // Modify order quantity
    assert(book.modify_order(1, 1500));
    
    level = book.get_best_bid();
    assert(level->total_quantity == 2000); // 1500 + 500
    assert(level->order_count == 2);
    
    // Cancel one order
    assert(book.cancel_order(2));
    
    level = book.get_best_bid();
    assert(level->total_quantity == 1500);
    assert(level->order_count == 1);
    assert(book.get_total_orders() == 1);
    
    // Cancel last order at this level
    assert(book.cancel_order(1));
    assert(book.get_best_bid() == nullptr);
    assert(book.get_total_orders() == 0);
    
    std::cout << "Order modifications: PASSED" << std::endl;
}

void test_multiple_price_levels() {
    std::cout << "Testing multiple price levels..." << std::endl;
    
    ObjectPool<Order> order_pool;
    OrderBook book(&order_pool);
    
    // Add multiple buy levels
    assert(book.add_order(1, 10000, 100, Side::BUY));
    assert(book.add_order(2, 9900, 200, Side::BUY));
    assert(book.add_order(3, 9800, 300, Side::BUY));
    
    // Add multiple sell levels
    assert(book.add_order(4, 10100, 150, Side::SELL));
    assert(book.add_order(5, 10200, 250, Side::SELL));
    assert(book.add_order(6, 10300, 350, Side::SELL));
    
    // Verify depth
    assert(book.get_bid_depth() == 3);
    assert(book.get_ask_depth() == 3);
    
    // Verify level access
    for (size_t i = 0; i < 3; ++i) {
        const PriceLevel* bid_level = book.get_bid_level(i);
        const PriceLevel* ask_level = book.get_ask_level(i);
        
        assert(bid_level != nullptr);
        assert(ask_level != nullptr);
        assert(bid_level->order_count == 1);
        assert(ask_level->order_count == 1);
    }
    
    // Test out of bounds access
    assert(book.get_bid_level(3) == nullptr);
    assert(book.get_ask_level(3) == nullptr);
    
    std::cout << "Multiple price levels: PASSED" << std::endl;
}

void test_order_book_clear() {
    std::cout << "Testing order book clear..." << std::endl;
    
    ObjectPool<Order> order_pool;
    OrderBook book(&order_pool);
    
    // Add several orders
    for (int i = 1; i <= 10; ++i) {
        book.add_order(i, 10000 + i * 10, 100, 
                      (i % 2) ? Side::BUY : Side::SELL);
    }
    
    assert(book.get_total_orders() == 10);
    assert(book.get_best_bid() != nullptr);
    assert(book.get_best_ask() != nullptr);
    
    // Clear the book
    book.clear();
    
    assert(book.get_total_orders() == 0);
    assert(book.get_best_bid() == nullptr);
    assert(book.get_best_ask() == nullptr);
    assert(book.get_bid_depth() == 0);
    assert(book.get_ask_depth() == 0);
    
    std::cout << "Order book clear: PASSED" << std::endl;
}

void test_duplicate_orders() {
    std::cout << "Testing duplicate order handling..." << std::endl;
    
    ObjectPool<Order> order_pool;
    OrderBook book(&order_pool);
    
    // Add order
    assert(book.add_order(1, 10000, 1000, Side::BUY));
    assert(book.get_total_orders() == 1);
    
    // Try to add same order ID again
    assert(!book.add_order(1, 10100, 500, Side::SELL));
    assert(book.get_total_orders() == 1);
    
    // Modify non-existent order
    assert(!book.modify_order(999, 2000));
    
    // Cancel non-existent order
    assert(!book.cancel_order(999));
    
    std::cout << "Duplicate order handling: PASSED" << std::endl;
}

int main() {
    std::cout << "Order Book Tests" << std::endl;
    std::cout << "================" << std::endl;
    
    test_basic_order_operations();
    test_price_level_ordering();
    test_order_modifications();
    test_multiple_price_levels();
    test_order_book_clear();
    test_duplicate_orders();
    
    std::cout << "\nAll tests PASSED!" << std::endl;
    return 0;
}
