cmake_minimum_required(VERSION 3.16)
project(hft-core VERSION 0.1.0 LANGUAGES CXX)

# C++20 required for concepts, coroutines, and latest optimizations
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags for maximum performance
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -flto -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -fsanitize=address,undefined")

# Find required packages
find_package(Threads REQUIRED)

# Option to build tests and benchmarks
option(BUILD_TESTS "Build test suite" ON)
option(BUILD_BENCHMARKS "Build benchmarks" ON)
option(BUILD_EXAMPLES "Build examples" ON)

# Core library (header-only)
add_library(hft-core INTERFACE)
target_include_directories(hft-core INTERFACE 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(hft-core INTERFACE Threads::Threads)

# Examples
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Benchmarks
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# Tests
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Installation
install(DIRECTORY core/ DESTINATION include/hft-core)