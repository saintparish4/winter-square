cmake_minimum_required(VERSION 3.20)
project(UltraLowLatencyMDE VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ultra-high performance compiler flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -mtune=native -flto -ffast-math")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -funroll-loops -fprefetch-loop-arrays")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fomit-frame-pointer -finline-functions")

# Additional optimizations for HFT
add_compile_options(-Wall -Wextra -Wpedantic)
add_compile_options(-mavx2 -mfma -mbmi -mbmi2)  # Modern CPU features

# Find required packages
find_package(Threads REQUIRED)

# Include directories
include_directories(include)
include_directories(src)

# Source files
set(SOURCES
    src/core/market_data_engine.cpp
    src/core/message_parser.cpp
    src/network/udp_receiver.cpp
    src/network/kernel_bypass.cpp
    src/memory/memory_pool.cpp
    src/memory/object_pool.cpp
    src/structures/lock_free_queue.cpp
    src/structures/order_book.cpp
    src/fpga/fpga_interface.cpp
    src/utils/high_precision_timer.cpp
    src/utils/cpu_affinity.cpp
)

# Main executable
add_executable(market_data_engine ${SOURCES} src/main.cpp)

# Link libraries
target_link_libraries(market_data_engine 
    Threads::Threads
    numa
    rt
)

# Benchmarks
add_executable(latency_benchmark 
    ${SOURCES}
    benchmarks/latency_benchmark.cpp
)
target_link_libraries(latency_benchmark 
    Threads::Threads
    numa
    rt
)

# Tests
enable_testing()
add_executable(unit_tests 
    ${SOURCES}
    tests/test_lock_free_queue.cpp
    tests/test_order_book.cpp
    tests/test_memory_pool.cpp
)
target_link_libraries(unit_tests 
    Threads::Threads
    numa
    rt
)
add_test(NAME UnitTests COMMAND unit_tests)

# Set runtime path for shared libraries
set_target_properties(market_data_engine PROPERTIES
    INSTALL_RPATH_USE_LINK_PATH TRUE
)
