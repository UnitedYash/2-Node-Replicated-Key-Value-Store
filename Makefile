CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -pthread

SRCS_SRV := server/server.cpp
HDRS_SRV := server/logger.h server/kv_store.h server/wal.h server/thread_pool.h
SRCS_CLI := client/client.cpp

.PHONY: all clean

all: kvserver kvclient

kvserver: $(SRCS_SRV) $(HDRS_SRV)
	$(CXX) $(CXXFLAGS) $(SRCS_SRV) -o $@

kvclient: $(SRCS_CLI)
	$(CXX) $(CXXFLAGS) $(SRCS_CLI) -o $@

clean:
	rm -f kvserver kvclient *.wal
