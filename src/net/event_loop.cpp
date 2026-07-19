// 事件循环实现
// TODO: 实现 epoll 事件循环
#include "gateway/net/event_loop.h"

#include <iostream>
#include <unistd.h>
#include <string.h>



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
        std::cerr << "EventLoop::add_fd epoll_ctl ADD fd=" << fd 
        << " failed" << strerror(errno) << std::endl;
    }
}

void EventLoop::mod_fd(int fd, uint32_t events) {
    registrations_[fd] = events;
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = events;
    if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        registrations_.erase(fd);
        std::cerr << "EventLoop::mod_fd epoll_ctl MOD fd=" << fd 
        << " failed" << strerror(errno) << std::endl;
    }
}
        
void EventLoop::del_fd(int fd) {
    registrations_.erase(fd);
    callbacks_.erase(fd);
    if (epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        std::cerr << "EventLoop::del_fd epoll_ctl DEL fd=" << fd 
        << " failed" << strerror(errno) << std::endl;
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

void EventLoop::stop() {
    running_.store(false);
}

void EventLoop::run(int timeout_ms) {
    running_.store(true);
    while (running_.load()) {
        int num_events = epoll_wait(
            epfd_, 
            events_.data(), 
            static_cast<int>(max_events_), 
            timeout_ms
        );
        if (num_events < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "EventLoop::run epoll_wait failed" << strerror(errno) << std::endl;
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
    }
}
