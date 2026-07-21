#pragma once

#include <memory>
#include <string>

class AsyncLogger;

// 日志级别
enum class LogLevel {
    TRACE = 0,
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL
};

// 日志器——线程安全的同步/异步日志前端
//
// 用法:
//   Logger::instance().set_level(LogLevel::INFO);
//   Logger::instance().set_async_logger(std::make_unique<AsyncLogger>("logs/gateway.log"));
//   LOG_INFO("server started on port %d", 8080);
class Logger {
public:
    static Logger& instance();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // 最低输出级别（低于此级别的日志不打印）
    void set_level(LogLevel level) { level_ = level; }
    LogLevel level() const { return level_; }

    

    // 设置异步日志后端（设置后日志将异步写入）
    void set_async_logger(std::unique_ptr<AsyncLogger> async_logger);

    // 核心方法
    void log(LogLevel level, const char* file, int line,
             const char* fmt, ...);

private:
    Logger() = default;
    ~Logger() = default;

    LogLevel level_ = LogLevel::INFO;
    std::unique_ptr<AsyncLogger> async_logger_;

    static const char* level_str(LogLevel level);
    static std::string timestamp();
};

// ============== 日志宏 ==============

#define LOG_TRACE(fmt, ...) \
    Logger::instance().log(LogLevel::TRACE, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_DEBUG(fmt, ...) \
    Logger::instance().log(LogLevel::DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_INFO(fmt, ...) \
    Logger::instance().log(LogLevel::INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_WARN(fmt, ...) \
    Logger::instance().log(LogLevel::WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    Logger::instance().log(LogLevel::ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_FATAL(fmt, ...) \
    Logger::instance().log(LogLevel::FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
