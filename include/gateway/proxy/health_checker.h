#pragma once

#include "gateway/core/config.h"  // Backend

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// 健康检查器：周期性 TCP 连接检测后端存活状态
//
// 用法：
//   HealthChecker checker;
//   checker.set_interval_ms(5000);
//   checker.set_connect_timeout_ms(2000);
//   checker.set_on_status_change([](const Backend& b, bool healthy) { ... });
//   checker.add_backend({"127.0.0.1", 9001});
//   checker.add_backend({"127.0.0.1", 9002});
//
//   在定时器回调中调用：timer->run_every(5000, [&]{ checker.check_all(); });
//
// 线程安全：add_backend / remove_backend 可以从任意线程调用
//         check_all 应从主线程调用

class HealthChecker {
public:
    // 状态变更回调：backend + 是否变为健康
    using StatusChangeCallback = std::function<void(const Backend& backend, bool healthy)>;

    HealthChecker() = default;

    HealthChecker(const HealthChecker&) = delete;
    HealthChecker& operator=(const HealthChecker&) = delete;

    // ---- 配置 ----
    void set_interval_ms(int ms) { interval_ms_ = ms; }
    void set_connect_timeout_ms(int ms) { connect_timeout_ms_ = ms; }
    void set_on_status_change(StatusChangeCallback cb) { on_status_change_ = std::move(cb); }

    // ---- 后端管理 ----
    void add_backend(const Backend& backend);
    void remove_backend(const Backend& backend);

    // ---- 健康检查 ----
    // 对所有已注册后端执行一次 TCP 连接健康检查
    void check_all();

    // ---- 查询 ----
    bool is_healthy(const Backend& backend) const;

private:
    struct BackendHealth {
        Backend backend;
        bool healthy = false;
        std::chrono::steady_clock::time_point last_check;
        int consecutive_failures = 0;
    };

    static std::string make_key(const std::string& host, int port);
    static std::string make_key(const Backend& backend);

    bool tcp_check(const Backend& backend) const;

    int interval_ms_ = 5000;
    int connect_timeout_ms_ = 2000;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, BackendHealth> backends_;
    StatusChangeCallback on_status_change_;
};
