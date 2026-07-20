#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <vector>

// 定时器：基于时间堆的高效定时任务管理
//
// 特性：
//   - 一次性定时器：run_after(delay_ms, cb)
//   - 周期定时器：   run_every(interval_ms, cb)
//   - 支持取消：     cancel(timer_id)（惰性删除）
//   - 线程安全：     可从任意线程添加/取消，tick() 在主线程调用
//
// 用法：
//   Timer timer;
//   auto id = timer.run_every(5000, []{ LOG_INFO("heartbeat"); });
//   timer.run_after(10000, []{ LOG_INFO("timeout"); });
//   ...
//   timer.tick();  // 在主循环中周期性调用
class Timer {
public:
    using TimerCallback = std::function<void()>;

    Timer() = default;
    ~Timer() = default;

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    // 延迟 delay_ms 后执行一次回调，返回定时器 ID
    uint64_t run_after(int64_t delay_ms, TimerCallback cb);

    // 每 interval_ms 执行一次回调，返回定时器 ID
    uint64_t run_every(int64_t interval_ms, TimerCallback cb);

    // 取消指定定时器（如果尚未触发）
    void cancel(uint64_t timer_id);

    // 处理所有已到期的定时器（必须在主线程周期性调用）
    void tick();

    // 获取距离下一个定时器到期的剩余毫秒数
    // 返回 -1 表示没有待处理的定时器
    int64_t next_timeout_ms() const;

    // 待处理的定时器数量（含已取消但未惰性删除的）
    size_t pending_count() const;

private:
    struct TimerEntry {
        uint64_t id;
        std::chrono::steady_clock::time_point expire_time;
        int64_t interval_ms;        // 0 = 一次性，>0 = 周期（毫秒）
        TimerCallback callback;
        bool cancelled = false;

        // 最小堆：按到期时间升序
        bool operator>(const TimerEntry& other) const {
            return expire_time > other.expire_time;
        }
    };

    mutable std::mutex mutex_;
    // 用 vector + push_heap/pop_heap 替代 std::priority_queue，
    // 避免 reinterpret_cast 访问底层容器的未定义行为
    std::vector<TimerEntry> heap_;
    std::atomic<uint64_t> next_id_{1};
};
