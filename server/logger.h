#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>

enum class LogLevel { INFO, WARN, ERR };

class Logger {
public:
    static void info(const std::string& msg)  { log(LogLevel::INFO, msg); }
    static void warn(const std::string& msg)  { log(LogLevel::WARN, msg); }
    static void error(const std::string& msg) { log(LogLevel::ERR,  msg); }

private:
    static void log(LogLevel level, const std::string& msg) {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_r(&t, &tm);

        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << "[" << std::put_time(&tm, "%H:%M:%S") << "] "
                  << level_tag(level) << "  " << msg << "\n";
    }

    static constexpr const char* level_tag(LogLevel level) {
        switch (level) {
            case LogLevel::INFO: return "INFO ";
            case LogLevel::WARN: return "WARN ";
            case LogLevel::ERR:  return "ERROR";
        }
        return "?????";
    }

    static inline std::mutex mutex_;
};
