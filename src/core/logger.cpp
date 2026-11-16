#include "core/logger.hpp"

#include <glib/gstdio.h>

#include <thread>

#include "core/datetime.hpp"

namespace bg {

Logger& Logger::instance() {
    static Logger l;
    return l;
}

const char* logLevelName(LogLevel level) {
    switch(level) {
        case LogLevel::Debug: return "Debug";
        case LogLevel::Info: return "Info";
        case LogLevel::Warning: return "Warning";
        case LogLevel::Error: return "Error";
    }
    return "";
}

void Logger::setLogFile(const std::string& path) {
    std::lock_guard<std::mutex> l(m_mutex);
    if(m_fp) fclose(m_fp);
    m_fp = nullptr;
    m_file = path;
    if(path.empty()) return;
    ensureParent(path);
    // Keep one previous log around
    if(fileSize(path) > 2 * 1024 * 1024) g_rename(path.c_str(), (path + ".1").c_str());
    m_fp = g_fopen(path.c_str(), "a");
}

void Logger::log(LogLevel level, const std::string& message) {
    LogEntry e{nowMs(), level, message};
    {
        std::lock_guard<std::mutex> l(m_mutex);
        std::string time = DateTime::fromUnixMs(e.time, false).format("yyyy-MM-dd HH:mm:ss.zzz");
        if(m_fp && level >= m_fileLevel) {
            fprintf(m_fp, "[%s][%s] %s\n", time.c_str(), logLevelName(level), message.c_str());
            fflush(m_fp);
        }
        if(m_console && level >= m_consoleLevel) fprintf(stderr, "[%s] %s\n", logLevelName(level), message.c_str());
        if(level >= LogLevel::Info || m_history.size() < 5000) {
            m_history.push_back(e);
            while(m_history.size() > 5000) m_history.pop_front();
        }
    }
    if(logged.empty()) return;
    if(std::this_thread::get_id() == m_mainThread)
        logged.emit(e);
    else
        defer([this, e] { logged.emit(e); });
}

std::deque<LogEntry> Logger::history() const {
    std::lock_guard<std::mutex> l(m_mutex);
    return m_history;
}

void Logger::clearHistory() {
    std::lock_guard<std::mutex> l(m_mutex);
    m_history.clear();
}

void log(const std::string& message, LogLevel level) { Logger::instance().log(level, message); }

}  // namespace bg
