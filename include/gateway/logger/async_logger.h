#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 异步日志后端：双缓冲 + 后台线程落盘
//
// 使用方式：
//   AsyncLogger async_log("logs/gateway.log", 1000);
//   async_log.start();
//   async_log.append("some log line");  // 线程安全
//   async_log.stop();
class AsyncLogger {
public:
    // filepath:           日志文件路径
    // flush_interval_ms:  刷盘间隔（毫秒）
    // roll_size:          单文件上限（字节），0 表示不滚动
    explicit AsyncLogger(const std::string& filepath,
                         int flush_interval_ms = 1000,
                         size_t roll_size = 0);

    ~AsyncLogger();

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    void append(const std::string& line);
    void start();
    void stop();

    bool running() const { return running_; }

private:
    std::string filepath_;
    int flush_interval_ms_;
    size_t roll_size_;

    // 双缓冲
    std::vector<std::string> cur_buf_;
    std::vector<std::string> next_buf_;

    std::mutex mutex_;
    std::condition_variable cv_;

    std::thread backend_thread_;
    std::atomic<bool> running_{false};

    FILE* fp_ = nullptr;
    size_t written_bytes_ = 0;

    void backend_loop();
    void flush_buf(std::vector<std::string>& buf);
    void open_file();
    void roll_file();
};
