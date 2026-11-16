#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "core/util.hpp"

namespace bg {

enum class LogLevel { Debug = 0, Info = 1, Warning = 2, Error = 3 };

struct LogEntry {
    int64_t time;  // unix ms
    LogLevel level;
    std::string message;
};

class Logger {
public:
    static Logger& instance();
    void setLogFile(const std::string& path);
    const std::string& logFile() const { return m_file; }
    void setConsoleLevel(LogLevel level) { m_consoleLevel = level; }
    void setConsoleEnabled(bool enabled) { m_console = enabled; }
    void setFileLevel(LogLevel level) { m_fileLevel = level; }
    void log(LogLevel level, const std::string& message);
    std::deque<LogEntry> history() const;
    void clearHistory();

    // Emitted on the main loop thread
    Signal<const LogEntry&> logged;

private:
    Logger() = default;
    std::string m_file;
    FILE* m_fp = nullptr;
    LogLevel m_consoleLevel = LogLevel::Warning;
    LogLevel m_fileLevel = LogLevel::Debug;
    bool m_console = true;
    std::thread::id m_mainThread = std::this_thread::get_id();
    std::deque<LogEntry> m_history;
    mutable std::mutex m_mutex;
};

const char* logLevelName(LogLevel level);
void log(const std::string& message, LogLevel level = LogLevel::Info);
inline void logDebug(const std::string& m) { log(m, LogLevel::Debug); }
inline void logWarning(const std::string& m) { log(m, LogLevel::Warning); }
inline void logError(const std::string& m) { log(m, LogLevel::Error); }

}  // namespace bg
