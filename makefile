CXX = g++

CXXFLAGS = -std=c++17 -pthread -Wall -I./src

# Build Mode Flags
RELEASE_FLAGS = -O3 -DNDEBUG
DEBUG_FLAGS = -O0 -g

# Directories
SRC_DIR = src
TEST_DIR = test

# Grabs all .cpp files in the src folder
CORE_SRCS = $(wildcard $(SRC_DIR)/*.cpp)

# Default Target 
all: benchmark

# 1. High-Performance Release Build
benchmark: 
	@echo "Compiling Benchmark (RELEASE MODE: -O3 -DNDEBUG)..."
	$(CXX) $(CXXFLAGS) $(RELEASE_FLAGS) $(CORE_SRCS) $(TEST_DIR)/benchmark.cpp -o build/db_bench
	@echo "Done! Run with: ./build/db_bench"

# 2. Debug Build (For GDB and testing)
debug:
	@echo "Compiling Benchmark (DEBUG MODE: -g -O0)..."
	$(CXX) $(CXXFLAGS) $(DEBUG_FLAGS) $(CORE_SRCS) $(TEST_DIR)/benchmark.cpp -o build/db_bench_debug
	@echo "Done! Run with: ./build/db_bench_debug"

# 3.  Correctness Test (basic)
test:
	@echo "Compiling Correctness Test (DEBUG MODE)..."
	$(CXX) $(CXXFLAGS) $(DEBUG_FLAGS) $(CORE_SRCS) $(TEST_DIR)/basic_test.cpp -o build/db_test
	@echo "Done! Run with: ./build/db_test"

# Cleanup
clean:
	rm -f build/db_bench build/db_bench_debug build/db_test
	rm -f ./data/imdb.aof
	@echo "Cleaned build artifacts."