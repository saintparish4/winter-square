#include "fpga/fpga_interface.hpp"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstring>

namespace hft {

bool FPGAInterface::initialize() noexcept {
    if (initialized_) {
        return true;
    }
    
    // Open FPGA device
    fpga_fd_ = open(config_.device_path, O_RDWR);
    if (fpga_fd_ < 0) {
        // FPGA not available, this is expected in most environments
        return false;
    }
    
    // Setup DMA buffers
    if (!setup_dma_buffers()) {
        close(fpga_fd_);
        fpga_fd_ = -1;
        return false;
    }
    
    // Program FPGA with our bitstream
    if (!program_fpga()) {
        cleanup_dma_buffers();
        close(fpga_fd_);
        fpga_fd_ = -1;
        return false;
    }
    
    initialized_ = true;
    return true;
}

void FPGAInterface::cleanup() noexcept {
    if (!initialized_) {
        return;
    }
    
    cleanup_dma_buffers();
    
    if (fpga_fd_ >= 0) {
        close(fpga_fd_);
        fpga_fd_ = -1;
    }
    
    initialized_ = false;
}

bool FPGAInterface::setup_dma_buffers() noexcept {
    // Allocate DMA-coherent memory
    dma_memory_ = mmap(nullptr, config_.dma_buffer_size,
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_LOCKED,
                      fpga_fd_, 0);
    
    if (dma_memory_ == MAP_FAILED) {
        return false;
    }
    
    // Split DMA memory into RX and TX buffers
    size_t buffer_size = config_.dma_buffer_size / 2;
    rx_buffer_ = static_cast<DMABuffer*>(dma_memory_);
    tx_buffer_ = reinterpret_cast<DMABuffer*>(
        static_cast<char*>(dma_memory_) + buffer_size);
    
    // Initialize buffer headers
    rx_buffer_->head.store(0, std::memory_order_relaxed);
    rx_buffer_->tail.store(0, std::memory_order_relaxed);
    tx_buffer_->head.store(0, std::memory_order_relaxed);
    tx_buffer_->tail.store(0, std::memory_order_relaxed);
    
    return true;
}

void FPGAInterface::cleanup_dma_buffers() noexcept {
    if (dma_memory_ && dma_memory_ != MAP_FAILED) {
        munmap(dma_memory_, config_.dma_buffer_size);
        dma_memory_ = nullptr;
    }
    
    rx_buffer_ = nullptr;
    tx_buffer_ = nullptr;
}

bool FPGAInterface::program_fpga() noexcept {
    // In a real implementation, this would load the FPGA bitstream
    // For demonstration, we'll just return true
    return true;
}

bool FPGAInterface::send_message_to_fpga(const HWMessage& msg) noexcept {
    if (!initialized_ || !tx_buffer_) {
        return false;
    }
    
    if (tx_buffer_->is_full()) {
        return false;
    }
    
    uint32_t tail = tx_buffer_->tail.load(std::memory_order_relaxed);
    tx_buffer_->messages[tail] = msg;
    
    // Update tail pointer
    tx_buffer_->tail.store((tail + 1) % DMABuffer::BUFFER_SIZE, 
                          std::memory_order_release);
    
    messages_processed_.fetch_add(1, std::memory_order_relaxed);
    dma_transfers_.fetch_add(1, std::memory_order_relaxed);
    
    return true;
}

FPGAInterface::HWMessage* FPGAInterface::receive_from_fpga() noexcept {
    if (!initialized_ || !rx_buffer_) {
        return nullptr;
    }
    
    if (rx_buffer_->is_empty()) {
        return nullptr;
    }
    
    uint32_t head = rx_buffer_->head.load(std::memory_order_relaxed);
    HWMessage* msg = &rx_buffer_->messages[head];
    
    // Update head pointer
    rx_buffer_->head.store((head + 1) % DMABuffer::BUFFER_SIZE,
                          std::memory_order_release);
    
    return msg;
}

bool FPGAInterface::add_order_hw(SymbolId symbol, OrderId order_id,
                                Price price, Quantity quantity, Side side) noexcept {
    HWMessage msg;
    msg.sequence_number = messages_processed_.load(std::memory_order_relaxed);
    msg.hw_timestamp = std::chrono::high_resolution_clock::now();
    msg.type = MessageType::ORDER_ADD;
    msg.symbol_id = symbol;
    msg.order_data.order_id = order_id;
    msg.order_data.price = price;
    msg.order_data.quantity = quantity;
    msg.order_data.side = side;
    
    return send_message_to_fpga(msg);
}

bool FPGAInterface::modify_order_hw(OrderId order_id, Quantity new_quantity) noexcept {
    HWMessage msg;
    msg.sequence_number = messages_processed_.load(std::memory_order_relaxed);
    msg.hw_timestamp = std::chrono::high_resolution_clock::now();
    msg.type = MessageType::ORDER_MODIFY;
    msg.order_data.order_id = order_id;
    msg.order_data.quantity = new_quantity;
    
    return send_message_to_fpga(msg);
}

bool FPGAInterface::cancel_order_hw(OrderId order_id) noexcept {
    HWMessage msg;
    msg.sequence_number = messages_processed_.load(std::memory_order_relaxed);
    msg.hw_timestamp = std::chrono::high_resolution_clock::now();
    msg.type = MessageType::ORDER_DELETE;
    msg.order_data.order_id = order_id;
    
    return send_message_to_fpga(msg);
}

FPGAInterface::BestQuote FPGAInterface::get_best_quote_hw(SymbolId symbol) const noexcept {
    // In a real implementation, this would read from FPGA memory-mapped registers
    (void)symbol;
    return {0, 0, 0, 0, false};
}

bool FPGAInterface::check_risk_hw(SymbolId symbol, Side side,
                                 Quantity quantity, Price price) const noexcept {
    // Hardware risk checks would be implemented in FPGA logic
    (void)symbol; (void)side; (void)quantity; (void)price;
    return true;
}

Duration FPGAInterface::get_processing_latency() const noexcept {
    // Return average processing latency from FPGA
    return Duration(100); // 100ns placeholder
}

FPGAInterface::FPGAStats FPGAInterface::get_stats() const noexcept {
    FPGAStats stats;
    stats.messages_processed = messages_processed_.load(std::memory_order_acquire);
    stats.hardware_errors = hardware_errors_.load(std::memory_order_acquire);
    stats.dma_transfers = dma_transfers_.load(std::memory_order_acquire);
    stats.fpga_temperature = 45; // Placeholder
    stats.fpga_utilization = 30; // Placeholder
    stats.avg_processing_time = Duration(100); // 100ns
    
    return stats;
}

void FPGAInterface::reset_stats() noexcept {
    messages_processed_.store(0, std::memory_order_release);
    hardware_errors_.store(0, std::memory_order_release);
    dma_transfers_.store(0, std::memory_order_release);
}

bool FPGAInterface::is_healthy() const noexcept {
    if (!initialized_) {
        return false;
    }
    
    // Check temperature and error rates
    uint32_t temp = get_temperature();
    return temp < 80; // Below 80°C
}

uint32_t FPGAInterface::get_temperature() const noexcept {
    // Read temperature from FPGA registers
    return 45; // Placeholder
}

uint32_t FPGAInterface::get_utilization() const noexcept {
    // Read utilization from FPGA registers
    return 30; // Placeholder
}

} // namespace hft
