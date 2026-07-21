#include "gateway/logger/logger.h"
#include "gateway/logger/async_logger.h"

#include <cstdarg>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <utility>

// ============== 单例 ==============

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::set_async_logger(std::unique_ptr<AsyncLogger> async_logger) {
    async_logger_ = std::move(async_logger);
    if (async_logger_) {
        async_logger_->start();
    }
}

// ============== 时间戳 ==============

std::string Logger::timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    struct tm tm_buf;
    localtime_r(&t, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

// ============== 级别转字符串 ==============

const char* Logger::level_str(LogLevel level) {
    switch (level) {
    case LogLevel::TRACE: return "TRACE";
    case LogLevel::DEBUG: return "DEBUG";
    case LogLevel::INFO:  return "INFO ";
    case LogLevel::WARN:  return "WARN ";
    case LogLevel::ERROR: return "ERROR";
    case LogLevel::FATAL: return "FATAL";
    }
    return "?????";
}

// ============== 核心日志方法 ==============

void Logger::log(LogLevel level, const char* file, int line,
                 const char* fmt, ...) {
    if (level < level_) return;

    const char* filename = file;
    for (const char* p = file; *p; ++p) {
        if (*p == '/') filename = p + 1;
    }

    va_list args;
    va_start(args, fmt);
    char msg_buf[4096];
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    std::ostringstream oss;
    oss << timestamp() << " ["
        << level_str(level) << "] "
        << filename << ":" << line << " "
        << msg_buf;

    if (async_logger_) {
        async_logger_->append(oss.str());
    } else {
        static std::mutex log_mutex;
        std::lock_guard<std::mutex> lock(log_mutex);
        fputs(oss.str().c_str(), stderr);
        fputs("\n", stderr);
        fflush(stderr);
    }
}
