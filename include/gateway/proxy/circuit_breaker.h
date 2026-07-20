#pragma once

#include <chrono>
#include <mutex>
#include <string>

// 断路器：防止网关持续向故障后端发送请求
//
// 状态机：
//   Closed ──(failures >= threshold)──> Open ──(timeout)──> HalfOpen
//   HalfOpen ──(success)───────────> Closed
//   HalfOpen ──(failure)───────────> Open
//
// 线程安全（process_request 在线程池中多线程调用）
class CircuitBreaker {
public:
    enum class State { Closed, Open, HalfOpen };

    // failure_threshold:     连续失败多少次后熔断
    // reset_timeout_ms:      熔断后多久进入半开状态
    // half_open_max_requests: 半开状态下最多允许几个试探请求
    CircuitBreaker(int failure_threshold = 5,
                   int reset_timeout_ms = 30000,
                   int half_open_max_requests = 3);

    CircuitBreaker(const CircuitBreaker&) = delete;
    CircuitBreaker& operator=(const CircuitBreaker&) = delete;

    // 请求是否允许通过
    // 闭路：总是允许
    // 开路：拒绝（快速失败）
    // 半开：允许前 N 个试探请求
    bool allow_request();

    // 上报结果
    void on_success();
    void on_failure();

    // 当前状态
    State state() const;

    // 状态名（用于日志）
    static const char* state_name(State s);

    // 统计
    int failure_count() const { return failure_count_; }
    int consecutive_failures() const { return consecutive_failures_; }

    // 重置到闭路（手动恢复）
    void reset();

private:
    int failure_threshold_;
    int reset_timeout_ms_;
    int half_open_max_requests_;

    State state_ = State::Closed;
    int failure_count_ = 0;          // 当前周期内的失败次数
    int consecutive_failures_ = 0;   // 连续失败次数
    int half_open_successes_ = 0;    // 半开状态下的成功次数
    int half_open_requests_ = 0;     // 半开状态下的请求数

    std::chrono::steady_clock::time_point opened_at_;  // 进入开路的时间

    mutable std::mutex mutex_;

    void transition_to_open();
    void try_transition_to_half_open();
};
