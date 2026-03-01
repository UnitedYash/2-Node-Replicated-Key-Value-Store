#pragma once

#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>

// Append-only Write-Ahead Log for crash recovery.
//
// Protocol on disk:
//   PUT  →  "PUT <key> <value_bytes>\n<value_bytes bytes of value>\n"
//   DEL  →  "DEL <key>\n"
//
// On startup, call replay() to restore state before accepting clients.
class WriteAheadLog {
public:
    explicit WriteAheadLog(const std::string& path)
        : path_(path),
          file_(path, std::ios::app | std::ios::binary) {}

    [[nodiscard]] bool is_open() const { return file_.is_open(); }

    void log_put(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        file_ << "PUT " << key << " " << value.size() << "\n";
        file_.write(value.data(), static_cast<std::streamsize>(value.size()));
        file_ << "\n";
        file_.flush();
    }

    void log_delete(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        file_ << "DEL " << key << "\n";
        file_.flush();
    }

    // Replay the log from disk, calling the appropriate callback for each entry.
    // The log file is opened read-only, independent of the append file handle.
    void replay(
        const std::function<void(const std::string&, const std::string&)>& on_put,
        const std::function<void(const std::string&)>&                     on_delete
    ) const {
        std::ifstream in(path_, std::ios::binary);
        if (!in) return;

        std::string line;
        while (std::getline(in, line)) {
            std::istringstream iss(line);
            std::string cmd;
            iss >> cmd;

            if (cmd == "PUT") {
                std::string key;
                size_t len = 0;
                iss >> key >> len;

                std::string value(len, '\0');
                in.read(value.data(), static_cast<std::streamsize>(len));
                in.ignore();   // consume trailing '\n' after value bytes
                on_put(key, value);

            } else if (cmd == "DEL") {
                std::string key;
                iss >> key;
                on_delete(key);
            }
        }
    }

private:
    std::string   path_;
    std::ofstream file_;
    mutable std::mutex mutex_;
};
