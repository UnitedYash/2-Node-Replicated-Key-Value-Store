#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

struct Stats {
    std::atomic<uint64_t> total_puts{0};
    std::atomic<uint64_t> total_gets{0};
    std::atomic<uint64_t> total_deletes{0};
    std::atomic<uint64_t> active_connections{0};
};

class KVStore {
public:
    // ── write operations ──────────────────────────────────────────────────────

    void put(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_[key] = value;
        ++stats_.total_puts;
    }

    // Restore from WAL replay — does not increment stats.
    void restore(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_[key] = value;
    }

    // Returns true if the key existed and was erased.
    bool remove(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.total_deletes;
        return data_.erase(key) > 0;
    }

    // Remove without incrementing stats (used during WAL replay).
    void restore_remove(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.erase(key);
    }

    // ── read operations ───────────────────────────────────────────────────────

    [[nodiscard]] std::optional<std::string> get(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.total_gets;
        if (auto it = data_.find(key); it != data_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    // ── connection tracking ───────────────────────────────────────────────────

    void on_connect()    { ++stats_.active_connections; }
    void on_disconnect() {
        if (stats_.active_connections > 0) --stats_.active_connections;
    }

    // ── stats ─────────────────────────────────────────────────────────────────

    [[nodiscard]] const Stats& stats() const { return stats_; }

private:
    std::unordered_map<std::string, std::string> data_;
    mutable std::mutex mutex_;
    Stats stats_;
};
