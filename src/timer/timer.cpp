#include "gateway/timer/timer.h"

#include <algorithm>

uint64_t Timer::run_after(int64_t delay_ms, TimerCallback cb) {
    uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);

    auto expire = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(delay_ms);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        heap_.push_back(TimerEntry{id, expire, 0, std::move(cb), false});
        std::push_heap(heap_.begin(), heap_.end(), std::greater<TimerEntry>{});
    }

    return id;
}

uint64_t Timer::run_every(int64_t interval_ms, TimerCallback cb) {
    uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);

    auto expire = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(interval_ms);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        heap_.push_back(TimerEntry{id, expire, interval_ms, std::move(cb), false});
        std::push_heap(heap_.begin(), heap_.end(), std::greater<TimerEntry>{});
    }

    return id;
}

void Timer::cancel(uint64_t timer_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& entry : heap_) {
        if (entry.id == timer_id) {
            entry.cancelled = true;
            entry.callback = nullptr;
            return;
        }
    }
}

void Timer::tick() {
    auto now = std::chrono::steady_clock::now();

    std::vector<TimerCallback> ready;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        while (!heap_.empty()) {
            if (heap_.front().expire_time > now) break;

            if (heap_.front().cancelled) {
                std::pop_heap(heap_.begin(), heap_.end(), std::greater<TimerEntry>{});
                heap_.pop_back();
                continue;
            }

            std::pop_heap(heap_.begin(), heap_.end(), std::greater<TimerEntry>{});
            auto entry = std::move(heap_.back());
            heap_.pop_back();

            if (entry.interval_ms > 0) {
                heap_.push_back(TimerEntry{
                    entry.id,
                    now + std::chrono::milliseconds(entry.interval_ms),
                    entry.interval_ms,
                    entry.callback,
                    false
                });
                std::push_heap(heap_.begin(), heap_.end(), std::greater<TimerEntry>{});
            }

            ready.push_back(std::move(entry.callback));
        }
    }

    for (auto& cb : ready) {
        if (cb) cb();
    }
}

int64_t Timer::next_timeout_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (heap_.empty()) return -1;

    // 跳过堆顶已取消的条目（不修改堆，tick() 会清除它们）
    for (const auto& entry : heap_) {
        if (!entry.cancelled) {
            auto now = std::chrono::steady_clock::now();
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                entry.expire_time - now).count();
            return remaining < 0 ? 0 : remaining;
        }
    }
    return -1;
}

size_t Timer::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return heap_.size();
}
