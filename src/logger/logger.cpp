#include "gateway/logger/logger.h"

#include <cstdarg>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>

// ============== 单例 ==============

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

// ============== 输出目标 ==============

void Logger::set_output(FILE* fp) {
    if (fp) fp_ = fp;
}

bool Logger::set_file(const std::string& filepath) {
    FILE* fp = fopen(filepath.c_str(), "a");
    if (!fp) return false;
    fp_ = fp;
    return true;
}

// ============== 时间戳 ==============

std::string Logger::timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S");
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

    // 提取文件名（去掉目录前缀）
    const char* filename = file;
    for (const char* p = file; *p; ++p) {
        if (*p == '/') filename = p + 1;
    }

    // 格式化用户消息
    va_list args;
    va_start(args, fmt);
    char msg_buf[4096];
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    // 组装整行日志
    std::ostringstream oss;
    oss << timestamp() << " ["
        << level_str(level) << "] "
        << filename << ":" << line << " "
        << msg_buf << "\n";

    // 线程安全输出
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);

    fputs(oss.str().c_str(), fp_);
    fflush(fp_);
}
