#include "gateway/net/event_loop.h"
#include "gateway/timer/timer.h"
#include "gateway/logger/logger.h"

#include <unistd.h>
#include <string.h>
#include <stdexcept>

EventLoop::EventLoop(int max_events) : epfd_(epoll_create1(0)), running_(false), max_events_(max_events) {
    if (epfd_ < 0) {
        throw std::runtime_error("failed to create epoll fd");
    }
    // 预分配事件队列内存
    events_.resize(max_events_);
}

EventLoop::~EventLoop() {
    if (epfd_ >= 0) {
        close(epfd_);
    }
}

void EventLoop::add_fd(int fd, uint32_t events) {
    registrations_[fd] = events;

    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = events;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        registrations_.erase(fd);
        LOG_ERROR("EventLoop::add_fd epoll_ctl ADD fd=%d failed: %s", fd, strerror(errno));
    }
}

void EventLoop::mod_fd(int fd, uint32_t events) {
    registrations_[fd] = events;
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = events;
    if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        registrations_.erase(fd);
        LOG_ERROR("EventLoop::mod_fd epoll_ctl MOD fd=%d failed: %s", fd, strerror(errno));
    }
}

void EventLoop::del_fd(int fd) {
    registrations_.erase(fd);
    callbacks_.erase(fd);
    if (epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        LOG_ERROR("EventLoop::del_fd epoll_ctl DEL fd=%d failed: %s", fd, strerror(errno));
    }
}

void EventLoop::set_callback(int fd, Callback cb) {
    callbacks_[fd] = std::move(cb);
}

void EventLoop::set_idle_callback(IdleCallback cb) {
    idle_cb_ = std::move(cb);
}

void EventLoop::remove_callback(int fd) {
    callbacks_.erase(fd);
}

void EventLoop::set_timer(Timer* timer) {
    timer_ = timer;
}

void EventLoop::stop() {
    running_.store(false);
}

int64_t EventLoop::compute_timeout_ms(int default_ms) const {
    if (!timer_) return default_ms;

    int64_t timer_timeout = timer_->next_timeout_ms();
    if (timer_timeout < 0) return default_ms;  // 无定时器

    // 取 default_ms 和 timer_timeout 中较小值，至少 1ms
    int64_t effective = timer_timeout < default_ms ? timer_timeout : default_ms;
    return effective > 0 ? effective : 1;
}

void EventLoop::run(int timeout_ms) {
    running_.store(true);
    while (running_.load()) {
        // 根据定时器到期时间动态调整 epoll_wait 超时
        int64_t effective_timeout = compute_timeout_ms(timeout_ms);

        int num_events = epoll_wait(
            epfd_,
            events_.data(),
            static_cast<int>(max_events_),
            static_cast<int>(effective_timeout)
        );
        if (num_events < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR("EventLoop::run epoll_wait failed: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < num_events; ++i) {
            int fd = events_[i].data.fd;
            uint32_t events = events_[i].events;

            auto it = callbacks_.find(fd);
            if (it != callbacks_.end()) {
                it->second(fd, events);
            }
        }

        // 每轮 epoll_wait 后执行空闲回调（用于定时器 tick、清理等）
        if (idle_cb_) {
            idle_cb_();
        }
    }
}
