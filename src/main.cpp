#include "core/market_data_engine.hpp"
#include "utils/cpu_affinity.hpp"
#include <iostream>
#include <signal.h>
#include <atomic>

using namespace hft;

std::atomic<bool> shutdown_requested{false};

void signal_handler(int signal) {
    std::cout << "Received signal " << signal << ", shutting down..." << std::endl;
    shutdown_requested.store(true, std::memory_order_release);
}

void print_statistics(const MarketDataEngine::Statistics& stats) {
    std::cout << "\n=== Market Data Engine Statistics ===" << std::endl;
    std::cout << "Messages Received: " << stats.messages_received << std::endl;
    std::cout << "Messages Processed: " << stats.messages_processed << std::endl;
    std::cout << "Parse Errors: " << stats.parse_errors << std::endl;
    std::cout << "Order Book Updates: " << stats.order_book_updates << std::endl;
    std::cout << "Active Symbols: " << stats.symbols_active << std::endl;
    
    std::cout << "\nProcessing Latency:" << std::endl;
    std::cout << "  Count: " << stats.processing_latency.count << std::endl;
    std::cout << "  Average: " << stats.processing_latency.avg_latency_ns << " ns" << std::endl;
    std::cout << "  Min: " << stats.processing_latency.min_latency_ns << " ns" << std::endl;
    std::cout << "  Max: " << stats.processing_latency.max_latency_ns << " ns" << std::endl;
    
    std::cout << "\nEnd-to-End Latency:" << std::endl;
    std::cout << "  Count: " << stats.end_to_end_latency.count << std::endl;
    std::cout << "  Average: " << stats.end_to_end_latency.avg_latency_ns << " ns" << std::endl;
    std::cout << "  Min: " << stats.end_to_end_latency.min_latency_ns << " ns" << std::endl;
    std::cout << "  Max: " << stats.end_to_end_latency.max_latency_ns << " ns" << std::endl;
}

int main(int argc, char* argv[]) {
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    std::cout << "Ultra-Low Latency Market Data Engine" << std::endl;
    std::cout << "====================================" << std::endl;
    
    // Configure main thread for high performance
    if (!configure_hft_thread(0)) {
        std::cerr << "Warning: Could not configure main thread for HFT" << std::endl;
    }
    
    // Create engine configuration
    MarketDataEngine::Config config;
    
    // Network configuration
    config.network_config.interface_ip = "0.0.0.0";
    config.network_config.port = 12345;
    config.network_config.cpu_affinity = 2;
    config.network_config.socket_buffer_size = 64 * 1024 * 1024;
    config.network_config.enable_kernel_bypass = true;
    config.network_config.enable_busy_polling = true;
    
    // Processing configuration
    config.processing_cpu = 1;
    config.network_cpu = 2;
    config.enable_fpga = false; // Disable FPGA for demo
    config.enable_order_book_processing = true;
    config.enable_latency_measurement = true;
    
    // Memory configuration
    config.message_pool_size = 1000000;
    config.order_pool_size = 10000000;
    config.max_symbols = 10000;
    
    // Create and initialize the engine
    auto engine = create_hft_engine(config);
    if (!engine) {
        std::cerr << "Failed to create market data engine" << std::endl;
        return 1;
    }
    
    if (!engine->initialize()) {
        std::cerr << "Failed to initialize market data engine" << std::endl;
        return 1;
    }
    
    // Set up callbacks
    engine->set_quote_callback([](SymbolId symbol, const PriceLevel* bid, const PriceLevel* ask) {
        if (bid && ask) {
            std::cout << "Symbol " << symbol << " - Bid: " << bid->price 
                     << "@" << bid->total_quantity 
                     << " Ask: " << ask->price << "@" << ask->total_quantity << std::endl;
        }
    });
    
    engine->set_trade_callback([](SymbolId symbol, Price price, Quantity quantity) {
        std::cout << "Trade - Symbol " << symbol << " Price: " << price 
                 << " Quantity: " << quantity << std::endl;
    });
    
    engine->set_statistics_callback([](const MarketDataEngine::Statistics& stats) {
        static auto last_print = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_print).count() >= 10) {
            print_statistics(stats);
            last_print = now;
        }
    });
    
    // Add some test symbols
    engine->add_symbol(1); // AAPL
    engine->add_symbol(2); // MSFT
    engine->add_symbol(3); // GOOGL
    
    // Start the engine
    if (!engine->start()) {
        std::cerr << "Failed to start market data engine" << std::endl;
        return 1;
    }
    
    std::cout << "Market Data Engine started successfully!" << std::endl;
    std::cout << "Listening on port " << config.network_config.port << std::endl;
    std::cout << "Press Ctrl+C to shutdown..." << std::endl;
    
    // Main loop
    while (!shutdown_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Check engine health
        if (!engine->is_healthy()) {
            std::cerr << "Engine health check failed!" << std::endl;
            break;
        }
    }
    
    std::cout << "Shutting down engine..." << std::endl;
    engine->stop();
    engine->shutdown();
    
    // Print final statistics
    auto final_stats = engine->get_statistics();
    print_statistics(final_stats);
    
    std::cout << "Shutdown complete." << std::endl;
    return 0;
}
