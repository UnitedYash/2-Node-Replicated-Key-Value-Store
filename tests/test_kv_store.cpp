#include <gtest/gtest.h>
#include "kv_store.h"

#include <thread>
#include <vector>

// ── basic CRUD ────────────────────────────────────────────────────────────────

TEST(KVStoreTest, PutAndGet) {
    KVStore store;
    store.put("name", "alice");
    EXPECT_EQ(store.get("name"), "alice");
}

TEST(KVStoreTest, GetMissing) {
    KVStore store;
    EXPECT_EQ(store.get("missing"), std::nullopt);
}

TEST(KVStoreTest, PutOverwrite) {
    KVStore store;
    store.put("key", "v1");
    store.put("key", "v2");
    EXPECT_EQ(store.get("key"), "v2");
}

TEST(KVStoreTest, RemoveExisting) {
    KVStore store;
    store.put("key", "val");
    EXPECT_TRUE(store.remove("key"));
    EXPECT_EQ(store.get("key"), std::nullopt);
}

TEST(KVStoreTest, RemoveMissing) {
    KVStore store;
    EXPECT_FALSE(store.remove("nonexistent"));
}

TEST(KVStoreTest, BinaryValue) {
    KVStore store;
    std::string bin = "a\0b\nc";
    bin.resize(5);
    store.put("bin", bin);
    EXPECT_EQ(store.get("bin"), bin);
}

// ── WAL-replay helpers ────────────────────────────────────────────────────────

TEST(KVStoreTest, RestoreDoesNotIncrementPuts) {
    KVStore store;
    store.restore("key", "val");
    EXPECT_EQ(store.stats().total_puts.load(), 0u);
    EXPECT_EQ(store.get("key"), "val");
}

TEST(KVStoreTest, RestoreRemoveDoesNotIncrementDeletes) {
    KVStore store;
    store.restore("key", "val");
    store.restore_remove("key");
    EXPECT_EQ(store.stats().total_deletes.load(), 0u);
    EXPECT_EQ(store.get("key"), std::nullopt);
}

// ── stats ─────────────────────────────────────────────────────────────────────

TEST(KVStoreTest, StatsTracking) {
    KVStore store;
    store.put("a", "1");
    store.put("b", "2");
    (void)store.get("a");
    store.remove("b");

    EXPECT_EQ(store.stats().total_puts.load(),    2u);
    EXPECT_EQ(store.stats().total_gets.load(),    1u);
    EXPECT_EQ(store.stats().total_deletes.load(), 1u);
}

TEST(KVStoreTest, ConnectionTracking) {
    KVStore store;
    store.on_connect();
    store.on_connect();
    EXPECT_EQ(store.stats().active_connections.load(), 2u);
    store.on_disconnect();
    EXPECT_EQ(store.stats().active_connections.load(), 1u);
}

// ── thread safety ─────────────────────────────────────────────────────────────

TEST(KVStoreTest, ConcurrentWrites) {
    KVStore store;
    constexpr int THREADS = 8, OPS = 1000;

    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&store, i] {
            for (int j = 0; j < OPS; ++j)
                store.put("k" + std::to_string(i), std::to_string(j));
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(store.stats().total_puts.load(), static_cast<uint64_t>(THREADS * OPS));
}

TEST(KVStoreTest, ConcurrentReadWrite) {
    KVStore store;
    store.put("shared", "initial");

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&store, i] {
            for (int j = 0; j < 500; ++j)
                store.put("shared", std::to_string(i * 500 + j));
        });
        threads.emplace_back([&store] {
            for (int j = 0; j < 500; ++j)
                (void)store.get("shared");  // must not crash
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_NE(store.get("shared"), std::nullopt);
}
