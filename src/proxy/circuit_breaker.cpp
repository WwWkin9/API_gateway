#include "gateway/proxy/circuit_breaker.h"

// ============== 构造 ==============

CircuitBreaker::CircuitBreaker(int failure_threshold,
                               int reset_timeout_ms,
                               int half_open_max_requests)
    : failure_threshold_(failure_threshold)
    , reset_timeout_ms_(reset_timeout_ms)
    , half_open_max_requests_(half_open_max_requests)
{}

// ============== 主入口 ==============

bool CircuitBreaker::allow_request() {
    std::lock_guard<std::mutex> lock(mutex_);

    switch (state_) {
    case State::Closed:
        return true;

    case State::Open:
        // 检查是否到了可以尝试半开的时间
        if (std::chrono::steady_clock::now() - opened_at_ >=
            std::chrono::milliseconds(reset_timeout_ms_)) {
            try_transition_to_half_open();
            return true;  // 第一个半开请求
        }
        return false;  // 仍在熔断期，拒绝

    case State::HalfOpen:
        // 限制试探请求数量
        if (half_open_requests_ < half_open_max_requests_) {
            half_open_requests_++;
            return true;
        }
        return false;  // 半开请求已达上限，等其他请求结果
    }

    return false;
}

// ============== 结果上报 ==============

void CircuitBreaker::on_success() {
    std::lock_guard<std::mutex> lock(mutex_);

    consecutive_failures_ = 0;

    switch (state_) {
    case State::Closed:
        // 闭路下的成功：重置本周期失败计数
        failure_count_ = 0;
        break;

    case State::HalfOpen:
        half_open_successes_++;
        // 试探请求全部成功 → 关闭断路器
        if (half_open_successes_ >= half_open_requests_) {
            state_ = State::Closed;
            failure_count_ = 0;
            half_open_successes_ = 0;
            half_open_requests_ = 0;
        }
        break;

    case State::Open:
        // 开路状态下不应该有成功（请求都被拒绝了）
        break;
    }
}

void CircuitBreaker::on_failure() {
    std::lock_guard<std::mutex> lock(mutex_);

    consecutive_failures_++;
    failure_count_++;

    switch (state_) {
    case State::Closed:
        // 连续失败达到阈值 → 熔断
        if (failure_count_ >= failure_threshold_) {
            transition_to_open();
        }
        break;

    case State::HalfOpen:
        // 半开状态下任何失败 → 重新开路
        transition_to_open();
        break;

    case State::Open:
        // 已开路，更新失败时间（重置超时计时）
        opened_at_ = std::chrono::steady_clock::now();
        break;
    }
}

// ============== 状态转换 ==============

void CircuitBreaker::transition_to_open() {
    state_ = State::Open;
    opened_at_ = std::chrono::steady_clock::now();
    half_open_successes_ = 0;
    half_open_requests_ = 0;
}

void CircuitBreaker::try_transition_to_half_open() {
    state_ = State::HalfOpen;
    half_open_successes_ = 0;
    half_open_requests_ = 0;
}

// ============== 查询 ==============

CircuitBreaker::State CircuitBreaker::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

const char* CircuitBreaker::state_name(State s) {
    switch (s) {
    case State::Closed:   return "CLOSED";
    case State::Open:     return "OPEN";
    case State::HalfOpen: return "HALF_OPEN";
    }
    return "UNKNOWN";
}

void CircuitBreaker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = State::Closed;
    failure_count_ = 0;
    consecutive_failures_ = 0;
    half_open_successes_ = 0;
    half_open_requests_ = 0;
}
