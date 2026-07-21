#include "gateway/net/event_loop.h"
#include "gateway/timer/timer.h"
#include "gateway/logger/logger.h"

#include <unistd.h>
#include <string.h>
#include <stdexcept>
#include <sys/eventfd.h>

EventLoop::EventLoop(int max_events) : epfd_(epoll_create1(0)), running_(false), max_events_(max_events) {
    if (epfd_ < 0) {
        throw std::runtime_error("failed to create epoll fd");
    }
    // 预分配事件队列内存
    events_.resize(max_events_);

    // eventfd：worker 线程通过 defer() 提交任务后唤醒 epoll_wait
    wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd_ < 0) {
        close(epfd_);
        throw std::runtime_error("failed to create eventfd");
    }
    add_fd(wakeup_fd_, EPOLLIN | EPOLLET);
    set_callback(wakeup_fd_, [this](int fd, uint32_t) {
        uint64_t val;
        while (::read(fd, &val, sizeof(val)) == sizeof(val)) {}
        run_deferred_tasks();
    });
}

EventLoop::~EventLoop() {
    if (wakeup_fd_ >= 0) {
        close(wakeup_fd_);
    }
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
    // 唤醒可能阻塞在 epoll_wait 的循环线程
    if (wakeup_fd_ >= 0) {
        uint64_t val = 1;
        ssize_t n = ::write(wakeup_fd_, &val, sizeof(val));
        (void)n;
    }
}

// 跨线程提交任务：排队后通过 eventfd 唤醒事件循环线程执行
void EventLoop::defer(Task task) {
    {
        std::lock_guard<std::mutex> lock(defer_mutex_);
        deferred_tasks_.push_back(std::move(task));
    }
    uint64_t val = 1;
    ssize_t n = ::write(wakeup_fd_, &val, sizeof(val));
    (void)n;  // eventfd 满时已有挂起的唤醒，无需重复写
}

// 在事件循环线程批量执行 defer 任务（锁内交换，锁外执行，避免回调中 defer 死锁）
// 每轮最多执行 max_deferred_per_round_ 个任务，剩余暂存到 pending_tasks_，
// 防止大量 defer 任务一次性执行导致 event loop 长时间阻塞（饿死）
void EventLoop::run_deferred_tasks() {
    std::vector<Task> tasks;
    {
        std::lock_guard<std::mutex> lock(defer_mutex_);
        tasks.swap(deferred_tasks_);
    }

    // 先把上一轮残留的 pending 任务合并到 tasks 前面
    if (!pending_tasks_.empty()) {
        tasks.insert(tasks.begin(),
                     std::make_move_iterator(pending_tasks_.begin()),
                     std::make_move_iterator(pending_tasks_.end()));
        pending_tasks_.clear();
    }

    int batch_limit = max_deferred_per_round_;
    size_t total = tasks.size();

    if (batch_limit > 0 && static_cast<int>(total) > batch_limit) {
        // 只执行前 batch_limit 个，剩余暂存
        for (int i = 0; i < batch_limit; ++i) {
            tasks[i]();
        }
        for (size_t i = batch_limit; i < total; ++i) {
            pending_tasks_.push_back(std::move(tasks[i]));
        }
        // 还有剩余任务，唤醒下一轮继续处理
        uint64_t val = 1;
        ssize_t n = ::write(wakeup_fd_, &val, sizeof(val));
        (void)n;
    } else {
        for (auto& task : tasks) {
            task();
        }
    }
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
                // 先复制回调到栈上再执行，防止回调中调用 del_fd 销毁自身导致 use-after-free
                auto cb = it->second;
                cb(fd, events);
            }
        }

        // 每轮 epoll_wait 后执行空闲回调（用于定时器 tick、清理等）
        if (idle_cb_) {
            idle_cb_();
        }
    }
}
