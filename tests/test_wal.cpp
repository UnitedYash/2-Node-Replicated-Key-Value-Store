#include <gtest/gtest.h>
#include "wal.h"

#include <cstdio>
#include <map>
#include <string>

// Helper: unique temp paths per test to avoid cross-test interference.
static std::string tmp(const std::string& name) {
    return "/tmp/kvtest_" + name + ".wal";
}

// ── basic put / delete ────────────────────────────────────────────────────────

TEST(WALTest, PutReplay) {
    auto path = tmp("put");
    { WriteAheadLog w(path); w.log_put("name", "alice"); w.log_put("age", "30"); }

    std::map<std::string, std::string> data;
    WriteAheadLog w(path);
    w.replay([&](const auto& k, const auto& v) { data[k] = v; }, [](const auto&) {});

    EXPECT_EQ(data["name"], "alice");
    EXPECT_EQ(data["age"],  "30");
    std::remove(path.c_str());
}

TEST(WALTest, DeleteReplay) {
    auto path = tmp("del");
    { WriteAheadLog w(path); w.log_put("key", "val"); w.log_delete("key"); }

    std::map<std::string, std::string> data;
    WriteAheadLog w(path);
    w.replay(
        [&](const auto& k, const auto& v) { data[k] = v; },
        [&](const auto& k)                { data.erase(k); }
    );

    EXPECT_EQ(data.count("key"), 0u);
    std::remove(path.c_str());
}

TEST(WALTest, EmptyReplay) {
    auto path = tmp("empty");
    bool called = false;
    WriteAheadLog w(path);
    w.replay([&](const auto&, const auto&) { called = true; }, [](const auto&) {});
    EXPECT_FALSE(called);
    std::remove(path.c_str());
}

// ── binary-safe value encoding ────────────────────────────────────────────────

TEST(WALTest, BinaryValueWithEmbeddedNewline) {
    auto path = tmp("binary_nl");
    std::string value = "line1\nline2";
    { WriteAheadLog w(path); w.log_put("k", value); }

    std::string recovered;
    WriteAheadLog w(path);
    w.replay([&](const auto& k, const auto& v) { if (k == "k") recovered = v; },
             [](const auto&) {});

    EXPECT_EQ(recovered, value);
    std::remove(path.c_str());
}

TEST(WALTest, BinaryValueWithNullByte) {
    auto path = tmp("binary_null");
    std::string value = "ab";
    value += '\0';
    value += "cd";
    { WriteAheadLog w(path); w.log_put("k", value); }

    std::string recovered;
    WriteAheadLog w(path);
    w.replay([&](const auto& k, const auto& v) { if (k == "k") recovered = v; },
             [](const auto&) {});

    EXPECT_EQ(recovered, value);
    std::remove(path.c_str());
}

// ── larger log ────────────────────────────────────────────────────────────────

TEST(WALTest, HundredEntries) {
    auto path = tmp("hundred");
    {
        WriteAheadLog w(path);
        for (int i = 0; i < 100; ++i)
            w.log_put("k" + std::to_string(i), "v" + std::to_string(i));
    }

    std::map<std::string, std::string> data;
    WriteAheadLog w(path);
    w.replay([&](const auto& k, const auto& v) { data[k] = v; }, [](const auto&) {});

    EXPECT_EQ(data.size(), 100u);
    EXPECT_EQ(data["k0"],  "v0");
    EXPECT_EQ(data["k99"], "v99");
    std::remove(path.c_str());
}

TEST(WALTest, InterleavedPutsAndDeletes) {
    auto path = tmp("interleaved");
    {
        WriteAheadLog w(path);
        w.log_put("a", "1");
        w.log_put("b", "2");
        w.log_delete("a");
        w.log_put("c", "3");
        w.log_delete("b");
    }

    std::map<std::string, std::string> data;
    WriteAheadLog w(path);
    w.replay(
        [&](const auto& k, const auto& v) { data[k] = v; },
        [&](const auto& k)                { data.erase(k); }
    );

    EXPECT_EQ(data.count("a"), 0u);
    EXPECT_EQ(data.count("b"), 0u);
    EXPECT_EQ(data["c"], "3");
    std::remove(path.c_str());
}
