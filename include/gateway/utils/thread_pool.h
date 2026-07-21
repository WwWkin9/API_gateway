#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads, size_t max_queue_size = 0);
    ~ThreadPool();

    template <typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result_t<F, Args...>> {
        using return_type = typename std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        std::future<return_type> result = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return result;
    }

    // 提交任务，队列满时返回 false
    bool try_submit(std::function<void()> job);

    // 提交任务，队列满时阻塞等待（兼容旧接口）
    void submit(std::function<void()> job);

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable cv_not_full_;  // 队列非满时通知阻塞的提交者
    bool stop_;
    size_t max_queue_size_;  // 0 表示不限制
};