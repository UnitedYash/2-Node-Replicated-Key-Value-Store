CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -pthread

SRCS_SRV   := server/server.cpp
HDRS_SRV   := server/logger.h server/kv_store.h server/wal.h server/thread_pool.h
SRCS_CLI   := client/client.cpp
SRCS_BENCH := bench/bench.cpp

.PHONY: all clean test bench

all: kvserver kvclient

kvserver: $(SRCS_SRV) $(HDRS_SRV)
	$(CXX) $(CXXFLAGS) $(SRCS_SRV) -o $@

kvclient: $(SRCS_CLI)
	$(CXX) $(CXXFLAGS) $(SRCS_CLI) -o $@

kvbench: $(SRCS_BENCH)
	$(CXX) $(CXXFLAGS) $(SRCS_BENCH) -o $@

bench: kvbench
	@echo "Run:  ./kvbench [--ops N] [--port PORT] [--value-size BYTES]"
	@echo "      (requires a running kvserver)"

# Requires cmake >= 3.14.  Downloads Google Test automatically on first run.
test: $(HDRS_SRV)
	cmake -S tests -B tests/build -DCMAKE_BUILD_TYPE=Release
	cmake --build tests/build --parallel
	./tests/build/run_tests

clean:
	rm -f kvserver kvclient kvbench *.wal
	rm -rf tests/build
