#include <gtest/gtest.h>
#include "thread_pool.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <numeric>
#include <vector>

// ── correctness ───────────────────────────────────────────────────────────────

TEST(ThreadPoolTest, AllTasksExecute) {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(4);
        for (int i = 0; i < 100; ++i)
            pool.submit([&] { ++counter; });
    }  // destructor joins — all tasks complete before we check
    EXPECT_EQ(counter.load(), 100);
}

TEST(ThreadPoolTest, LargeBatch) {
    std::atomic<int> count{0};
    {
        ThreadPool pool(8);
        for (int i = 0; i < 10000; ++i)
            pool.submit([&] { ++count; });
    }
    EXPECT_EQ(count.load(), 10000);
}

TEST(ThreadPoolTest, SumIsCorrect) {
    constexpr int N = 1000;
    std::atomic<int64_t> sum{0};
    {
        ThreadPool pool(4);
        for (int i = 1; i <= N; ++i)
            pool.submit([&sum, i] { sum += i; });
    }
    EXPECT_EQ(sum.load(), static_cast<int64_t>(N) * (N + 1) / 2);
}

// ── concurrency ───────────────────────────────────────────────────────────────

TEST(ThreadPoolTest, TasksRunConcurrently) {
    std::atomic<int> current{0};
    std::atomic<int> peak{0};

    {
        ThreadPool pool(4);
        for (int i = 0; i < 4; ++i) {
            pool.submit([&] {
                int c = ++current;
                int prev = peak.load();
                while (c > prev && !peak.compare_exchange_weak(prev, c)) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                --current;
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GT(peak.load(), 1);  // at least 2 tasks ran simultaneously
}

TEST(ThreadPoolTest, SingleThreadExecutesAll) {
    std::atomic<int> count{0};
    {
        ThreadPool pool(1);
        for (int i = 0; i < 50; ++i)
            pool.submit([&] { ++count; });
    }
    EXPECT_EQ(count.load(), 50);
}

// ── thread-safe task submission ───────────────────────────────────────────────

TEST(ThreadPoolTest, ConcurrentSubmit) {
    std::atomic<int> count{0};
    {
        ThreadPool pool(4);
        std::vector<std::thread> submitters;
        for (int i = 0; i < 4; ++i) {
            submitters.emplace_back([&pool, &count] {
                for (int j = 0; j < 250; ++j)
                    pool.submit([&count] { ++count; });
            });
        }
        for (auto& t : submitters) t.join();
    }
    EXPECT_EQ(count.load(), 1000);
}
