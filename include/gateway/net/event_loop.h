#pragma once

// 事件循环：基于 epoll 的 Reactor 事件驱动核心
// TODO: 实现 epoll 事件循环

#include <sys/epoll.h>
#include <functional>
#include <vector>
#include <unordered_map>
#include <atomic>

#define EPOLL_MAX_EVENTS 512

class EventLoop {
public:
    using Callback = std::function<void(int fd, uint32_t events)>;
    static constexpr int kDefaultMaxEvents = EPOLL_MAX_EVENTS;
    explicit EventLoop(int max_events = kDefaultMaxEvents);
    ~EventLoop();

    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;

    void add_fd(int fd, uint32_t events);
    void mod_fd(int fd, uint32_t events);
    void del_fd(int fd);

    using IdleCallback = std::function<void()>;
    void set_idle_callback(IdleCallback cb);

    void set_callback(int fd, Callback cb);
    void remove_callback(int fd);

    void run(int timeout_ms = 1000);
    void stop();

    bool is_running() const { return running_; }
    int epfd() const { return epfd_; }

private:
    int epfd_;
    IdleCallback idle_cb_;
    std::atomic<bool> running_{false};
    int max_events_;
    std::vector<struct epoll_event> events_;                // epoll 事件队列
    std::unordered_map<int, Callback> callbacks_;
    std::unordered_map<int, uint32_t> registrations_;       // 注册的文件描述符和事件类型
};
