#include "gateway/logger/async_logger.h"

#include <cstdio>
#include <chrono>

AsyncLogger::AsyncLogger(const std::string& filepath,
                         int flush_interval_ms,
                         size_t roll_size)
    : filepath_(filepath)
    , flush_interval_ms_(flush_interval_ms)
    , roll_size_(roll_size)
{
    cur_buf_.reserve(4096);
    next_buf_.reserve(4096);
}

AsyncLogger::~AsyncLogger() {
    stop();
}

// ============== 生产者接口 ==============

void AsyncLogger::append(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    cur_buf_.push_back(line);

    // 缓冲区满时提前通知后台线程
    if (cur_buf_.size() >= 1024) {
        cv_.notify_one();
    }
}

void AsyncLogger::start() {
    running_ = true;
    open_file();
    backend_thread_ = std::thread(&AsyncLogger::backend_loop, this);
}

void AsyncLogger::stop() {
    if (!running_) return;

    running_ = false;
    cv_.notify_all();

    if (backend_thread_.joinable()) {
        backend_thread_.join();
    }

    if (fp_) {
        fclose(fp_);
        fp_ = nullptr;
    }
}

// ============== 后台线程 ==============

void AsyncLogger::backend_loop() {
    while (running_) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(flush_interval_ms_),
                [this]() { return !running_ || !cur_buf_.empty(); });

            // 交换双缓冲
            cur_buf_.swap(next_buf_);
        }

        if (!next_buf_.empty()) {
            flush_buf(next_buf_);
            next_buf_.clear();
        }
    }

    // 退出前刷新剩余日志
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cur_buf_.empty()) {
            cur_buf_.swap(next_buf_);
        }
    }
    if (!next_buf_.empty()) {
        flush_buf(next_buf_);
        next_buf_.clear();
    }
}

void AsyncLogger::flush_buf(std::vector<std::string>& buf) {
    for (const auto& line : buf) {
        size_t len = line.size();

        if (roll_size_ > 0 && written_bytes_ + len > roll_size_) {
            roll_file();
        }

        fwrite(line.data(), 1, len, fp_);
        fwrite("\n", 1, 1, fp_);
        written_bytes_ += len + 1;
    }
    fflush(fp_);
}

// ============== 文件管理 ==============

void AsyncLogger::open_file() {
    if (fp_) fclose(fp_);

    fp_ = fopen(filepath_.c_str(), "a");
    if (!fp_) {
        fprintf(stderr, "AsyncLogger: failed to open %s\n", filepath_.c_str());
        fp_ = stderr;
    } else {
        written_bytes_ = 0;
    }
}

void AsyncLogger::roll_file() {
    if (!fp_ || fp_ == stderr) return;

    fclose(fp_);

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y%m%d_%H%M%S", localtime(&t));

    std::string new_name = filepath_ + "." + time_buf;
    rename(filepath_.c_str(), new_name.c_str());

    open_file();
}
