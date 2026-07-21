#include "gateway/utils/thread_pool.h"

ThreadPool::ThreadPool(size_t num_threads, size_t max_queue_size)
    : stop_(false), max_queue_size_(max_queue_size) {
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this]() {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this]() {
                        return stop_ || !tasks_.empty();
                    });
                    if (stop_ && tasks_.empty()) return;
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                // 取完任务后通知可能阻塞在 try_submit 的提交者
                cv_not_full_.notify_one();
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
}

bool ThreadPool::try_submit(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) throw std::runtime_error("submit on stopped ThreadPool");
        if (max_queue_size_ > 0 && tasks_.size() >= max_queue_size_) {
            return false;  // 队列满，拒绝
        }
        tasks_.emplace(std::move(job));
    }
    cv_.notify_one();
    return true;
}

void ThreadPool::submit(std::function<void()> job) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stop_) throw std::runtime_error("submit on stopped ThreadPool");
        // 队列满时阻塞等待，直到有空位
        if (max_queue_size_ > 0) {
            cv_not_full_.wait(lock, [this]() {
                return stop_ || tasks_.size() < max_queue_size_;
            });
            if (stop_) throw std::runtime_error("submit on stopped ThreadPool");
        }
        tasks_.emplace(std::move(job));
    }
    cv_.notify_one();
}