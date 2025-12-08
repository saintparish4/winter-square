#!/bin/bash

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}HFT Core Build Script${NC}"
echo "===================="
echo ""

# Parse arguments
BUILD_TYPE="Release"
CLEAN=false
RUN_TESTS=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --clean)
            CLEAN=true
            shift
            ;;
        --test)
            RUN_TESTS=true
            shift
            ;;
        --help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --debug     Build with debug symbols and sanitizers"
            echo "  --clean     Clean build directory before building"
            echo "  --test      Run tests after building"
            echo "  --help      Show this help message"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Check for required tools
echo -e "${YELLOW}Checking dependencies...${NC}"

if ! command -v cmake &> /dev/null; then
    echo -e "${RED}CMake not found. Please install CMake 3.16 or later.${NC}"
    exit 1
fi

if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
    echo -e "${RED}C++ compiler not found. Please install g++ or clang++.${NC}"
    exit 1
fi

echo -e "${GREEN}✓ Dependencies OK${NC}"
echo ""

# Clean if requested
if [ "$CLEAN" = true ]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf build
    echo -e "${GREEN}✓ Clean complete${NC}"
    echo ""
fi

# Create build directory
mkdir -p build
cd build

# Configure
echo -e "${YELLOW}Configuring ($BUILD_TYPE)...${NC}"

if [ "$BUILD_TYPE" = "Debug" ]; then
    cmake .. \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -g" \
        -DBUILD_TESTS=ON \
        -DBUILD_BENCHMARKS=ON \
        -DBUILD_EXAMPLES=ON
else
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS_RELEASE="-O3 -march=native -mtune=native -flto -DNDEBUG" \
        -DBUILD_TESTS=ON \
        -DBUILD_BENCHMARKS=ON \
        -DBUILD_EXAMPLES=ON
fi

echo -e "${GREEN}✓ Configuration complete${NC}"
echo ""

# Build
echo -e "${YELLOW}Building...${NC}"
make -j$(nproc)
echo -e "${GREEN}✓ Build complete${NC}"
echo ""

# Run tests if requested
if [ "$RUN_TESTS" = true ]; then
    echo -e "${YELLOW}Running tests...${NC}"
    ctest --output-on-failure
    echo -e "${GREEN}✓ Tests passed${NC}"
    echo ""
fi

# Summary
echo -e "${GREEN}Build Summary${NC}"
echo "=============="
echo "Build type: $BUILD_TYPE"
echo "Build directory: $(pwd)"
echo ""
echo "Executables:"
echo "  Examples:"
echo "    - ./examples/basic_example"
echo "    - ./examples/udp_sender"
echo "  Benchmarks:"
echo "    - ./benchmarks/latency_benchmark"
echo "  Tests:"
echo "    - ./tests/test_lockfree_queue"
echo ""
echo -e "${GREEN}Build successful! 🚀${NC}"