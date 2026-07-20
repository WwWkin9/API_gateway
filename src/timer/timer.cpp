#include "gateway/timer/timer.h"

#include <algorithm>

uint64_t Timer::run_after(int64_t delay_ms, TimerCallback cb) {
    uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);

    auto expire = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(delay_ms);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        heap_.push(TimerEntry{id, expire, 0, std::move(cb), false});
    }

    return id;
}

uint64_t Timer::run_every(int64_t interval_ms, TimerCallback cb) {
    uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);

    auto expire = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(interval_ms);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        heap_.push(TimerEntry{id, expire, interval_ms, std::move(cb), false});
    }

    return id;
}

void Timer::cancel(uint64_t timer_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 惰性删除：遍历堆底层容器标记 cancelled
    // priority_queue 不暴露底层容器，需要技巧访问
    auto& container = *reinterpret_cast<
        std::vector<TimerEntry>*>(&heap_);

    for (auto& entry : container) {
        if (entry.id == timer_id) {
            entry.cancelled = true;
            entry.callback = nullptr;  // 释放回调中可能持有的资源
            return;
        }
    }
}

void Timer::tick() {
    auto now = std::chrono::steady_clock::now();

    // 收集所有到期的回调（在锁内收集，锁外执行，防止回调中 add/cancel 死锁）
    std::vector<TimerCallback> ready;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        while (!heap_.empty()) {
            const auto& top = heap_.top();
            if (top.expire_time > now) break;  // 还没到时间

            if (top.cancelled) {
                // 已取消，直接丢弃
                heap_.pop();
                continue;
            }

            // 取出到期的条目
            auto entry = heap_.top();
            heap_.pop();

            // 周期定时器：重新插入下一次触发
            if (entry.interval_ms > 0) {
                heap_.push(TimerEntry{
                    entry.id,
                    now + std::chrono::milliseconds(entry.interval_ms),
                    entry.interval_ms,
                    entry.callback,  // 周期回调需要保留引用
                    false
                });
            }

            ready.push_back(std::move(entry.callback));
        }
    }

    // 锁外执行回调（安全：回调中可以自由 add/cancel）
    for (auto& cb : ready) {
        if (cb) cb();
    }
}

int64_t Timer::next_timeout_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // 跳过堆顶已取消的条目
    auto& container = const_cast<std::vector<TimerEntry>&>(
        *reinterpret_cast<const std::vector<TimerEntry>*>(&heap_));

    // 注意：不能直接 pop，因为 priority_queue 的 const 限制
    // 改为：在 tick() 中清除，这里只跳过顶层已取消条目的影响
    // 对返回值取保守估计：如果顶层已取消，忽略它，查看下一个
    // 但 priority_queue 不支持查看第二个元素...
    // 简化处理：忽略顶层已取消导致的精度损失（已取消的顶层会在下次 tick 被清除）

    if (heap_.empty()) return -1;

    // 如果顶层已取消，返回 0 让 tick 尽快处理
    if (heap_.top().cancelled) return 0;

    auto now = std::chrono::steady_clock::now();
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        heap_.top().expire_time - now).count();

    return remaining < 0 ? 0 : remaining;
}

size_t Timer::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return heap_.size();
}
