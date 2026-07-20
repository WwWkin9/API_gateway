#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

// 网关统计：记录请求数量、延迟和错误率
//
// 线程安全：所有计数操作使用 std::atomic，可从线程池多线程调用
//
// 用法：
//   GatewayStats::instance().record_request("/api/user", 42);
//   GatewayStats::instance().record_error("/api/user");
//   auto s = GatewayStats::instance().global_snapshot();

class GatewayStats {
public:
    // 单条路由的统计数据（内部，包含 atomic 成员，不可拷贝）
    struct RouteStats {
        std::atomic<uint64_t> total_requests{0};
        std::atomic<uint64_t> success_count{0};
        std::atomic<uint64_t> error_count{0};
        std::atomic<uint64_t> latency_sum_ms{0};
        std::atomic<uint64_t> latency_max_ms{0};
        std::atomic<uint64_t> in_flight{0};

        double avg_latency_ms() const;
    };

    // 路由统计快照（不可变副本，不含 atomic）
    struct RouteSnapshot {
        uint64_t total_requests = 0;
        uint64_t success_count = 0;
        uint64_t error_count = 0;
        uint64_t latency_sum_ms = 0;
        uint64_t latency_max_ms = 0;
        uint64_t in_flight = 0;
        double avg_latency_ms = 0.0;
    };

    // 全局统计快照
    struct Snapshot {
        uint64_t total_requests = 0;
        uint64_t total_success = 0;
        uint64_t total_errors = 0;
        double avg_latency_ms = 0.0;
        uint64_t max_latency_ms = 0;
        uint64_t in_flight = 0;
        uint64_t upstream_errors = 0;
        uint64_t circuit_breaker_trips = 0;
        uint64_t rate_limited = 0;
    };

    static GatewayStats& instance();

    GatewayStats(const GatewayStats&) = delete;
    GatewayStats& operator=(const GatewayStats&) = delete;

    // ---- 请求计数 ----
    void record_request(const std::string& route, int64_t latency_ms);
    void record_success(const std::string& route);
    void record_error(const std::string& route);

    // ---- 全局计数器 ----
    void inc_upstream_errors() { upstream_errors_.fetch_add(1, std::memory_order_relaxed); }
    void inc_circuit_breaker_trips() { cb_trips_.fetch_add(1, std::memory_order_relaxed); }
    void inc_rate_limited() { rate_limited_.fetch_add(1, std::memory_order_relaxed); }

    uint64_t upstream_errors() const { return upstream_errors_.load(std::memory_order_relaxed); }
    uint64_t circuit_breaker_trips() const { return cb_trips_.load(std::memory_order_relaxed); }
    uint64_t rate_limited() const { return rate_limited_.load(std::memory_order_relaxed); }

    // ---- 快照与重置 ----
    std::unordered_map<std::string, RouteSnapshot> route_snapshot() const;
    Snapshot global_snapshot() const;

    // 重置所有计数器
    void reset_all();

private:
    GatewayStats() = default;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, RouteStats> routes_;

    std::atomic<uint64_t> upstream_errors_{0};
    std::atomic<uint64_t> cb_trips_{0};
    std::atomic<uint64_t> rate_limited_{0};

    RouteStats& get_or_create(const std::string& route);
};
