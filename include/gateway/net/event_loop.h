#pragma once

#include <sys/epoll.h>
#include <functional>
#include <vector>
#include <deque>
#include <unordered_map>
#include <atomic>
#include <mutex>

// 前向声明
class Timer;

#define EPOLL_MAX_EVENTS 512

class EventLoop {
public:
    using Callback = std::function<void(int fd, uint32_t events)>;
    using Task = std::function<void()>;
    static constexpr int kDefaultMaxEvents = EPOLL_MAX_EVENTS;
    explicit EventLoop(int max_events = kDefaultMaxEvents);
    ~EventLoop();

    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;

    // 以下 add/mod/del 必须在事件循环线程调用
    void add_fd(int fd, uint32_t events);
    void mod_fd(int fd, uint32_t events);
    void del_fd(int fd);

    using IdleCallback = std::function<void()>;
    void set_idle_callback(IdleCallback cb);

    void set_callback(int fd, Callback cb);
    void remove_callback(int fd);

    // 跨线程安全：把任务排队到事件循环线程执行（通过 eventfd 唤醒 epoll_wait）
    void defer(Task task);

    // 设置定时器：epoll_wait 会根据最近的定时器到期时间动态调整超时值
    void set_timer(Timer* timer);

    // 设置每轮最多执行的 defer 任务数（防饿死，默认 256，0 表示不限制）
    void set_max_deferred_per_round(int n) { max_deferred_per_round_ = n; }

    void run(int timeout_ms = 1000);
    void stop();

    bool is_running() const { return running_; }
    int epfd() const { return epfd_; }

private:
    int epfd_;
    IdleCallback idle_cb_;
    std::atomic<bool> running_{false};
    int max_events_;
    std::vector<struct epoll_event> events_;
    std::unordered_map<int, Callback> callbacks_;
    std::unordered_map<int, uint32_t> registrations_;

    Timer* timer_ = nullptr;

    // defer 任务队列（eventfd 用于唤醒阻塞在 epoll_wait 的循环线程）
    int wakeup_fd_ = -1;
    std::mutex defer_mutex_;
    std::vector<Task> deferred_tasks_;

    // 分批执行：超过 max_deferred_per_round_ 的任务暂存于此（仅事件循环线程访问）
    std::deque<Task> pending_tasks_;
    int max_deferred_per_round_ = 256;

    // 计算本次 epoll_wait 的有效超时（结合定时器到期时间）
    int64_t compute_timeout_ms(int default_ms) const;

    // 执行所有已排队的 defer 任务
    void run_deferred_tasks();
};
