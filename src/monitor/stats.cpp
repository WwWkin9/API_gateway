#include "gateway/monitor/stats.h"

// ============== RouteStats ==============

double GatewayStats::RouteStats::avg_latency_ms() const {
    uint64_t total = total_requests.load(std::memory_order_relaxed);
    if (total == 0) return 0.0;
    uint64_t sum = latency_sum_ms.load(std::memory_order_relaxed);
    return static_cast<double>(sum) / static_cast<double>(total);
}

// ============== 单例 ==============

GatewayStats& GatewayStats::instance() {
    static GatewayStats inst;
    return inst;
}

// ============== 路由统计访问 ==============

GatewayStats::RouteStats& GatewayStats::get_or_create(const std::string& route) {
    {
        auto it = routes_.find(route);
        if (it != routes_.end()) {
            return it->second;
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    return routes_.try_emplace(route).first->second;
}

// ============== 请求计数 ==============

void GatewayStats::record_request(const std::string& route, int64_t latency_ms) {
    auto& rs = get_or_create(route);

    rs.total_requests.fetch_add(1, std::memory_order_relaxed);
    rs.latency_sum_ms.fetch_add(static_cast<uint64_t>(latency_ms), std::memory_order_relaxed);

    uint64_t current_max = rs.latency_max_ms.load(std::memory_order_relaxed);
    uint64_t new_val = static_cast<uint64_t>(latency_ms);
    while (new_val > current_max &&
           !rs.latency_max_ms.compare_exchange_weak(current_max, new_val,
                                                     std::memory_order_relaxed)) {
    }

    if (rs.in_flight.load(std::memory_order_relaxed) > 0) {
        rs.in_flight.fetch_sub(1, std::memory_order_relaxed);
    }
}

void GatewayStats::record_success(const std::string& route) {
    auto& rs = get_or_create(route);
    rs.success_count.fetch_add(1, std::memory_order_relaxed);
}

void GatewayStats::record_error(const std::string& route) {
    auto& rs = get_or_create(route);
    rs.error_count.fetch_add(1, std::memory_order_relaxed);
}

// ============== 快照 ==============

std::unordered_map<std::string, GatewayStats::RouteSnapshot>
GatewayStats::route_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::unordered_map<std::string, RouteSnapshot> result;
    result.reserve(routes_.size());

    for (const auto& [name, rs] : routes_) {
        RouteSnapshot snap;
        snap.total_requests = rs.total_requests.load(std::memory_order_relaxed);
        snap.success_count  = rs.success_count.load(std::memory_order_relaxed);
        snap.error_count    = rs.error_count.load(std::memory_order_relaxed);
        snap.latency_sum_ms = rs.latency_sum_ms.load(std::memory_order_relaxed);
        snap.latency_max_ms = rs.latency_max_ms.load(std::memory_order_relaxed);
        snap.in_flight      = rs.in_flight.load(std::memory_order_relaxed);
        if (snap.total_requests > 0) {
            snap.avg_latency_ms = static_cast<double>(snap.latency_sum_ms) /
                                  static_cast<double>(snap.total_requests);
        }
        result.emplace(name, std::move(snap));
    }

    return result;
}

GatewayStats::Snapshot GatewayStats::global_snapshot() const {
    Snapshot snap;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t total_req = 0;
        uint64_t total_succ = 0;
        uint64_t total_err = 0;
        uint64_t total_lat = 0;
        uint64_t max_lat = 0;
        uint64_t inflight = 0;

        for (const auto& [name, rs] : routes_) {
            total_req += rs.total_requests.load(std::memory_order_relaxed);
            total_succ += rs.success_count.load(std::memory_order_relaxed);
            total_err  += rs.error_count.load(std::memory_order_relaxed);
            total_lat  += rs.latency_sum_ms.load(std::memory_order_relaxed);
            inflight   += rs.in_flight.load(std::memory_order_relaxed);

            uint64_t route_max = rs.latency_max_ms.load(std::memory_order_relaxed);
            if (route_max > max_lat) max_lat = route_max;
        }

        snap.total_requests = total_req;
        snap.total_success = total_succ;
        snap.total_errors = total_err;
        snap.max_latency_ms = max_lat;
        snap.in_flight = inflight;

        if (total_req > 0) {
            snap.avg_latency_ms = static_cast<double>(total_lat) /
                                  static_cast<double>(total_req);
        }
    }

    snap.upstream_errors       = upstream_errors_.load(std::memory_order_relaxed);
    snap.circuit_breaker_trips = cb_trips_.load(std::memory_order_relaxed);
    snap.rate_limited          = rate_limited_.load(std::memory_order_relaxed);

    return snap;
}

// ============== 重置 ==============

void GatewayStats::reset_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    routes_.clear();
    upstream_errors_.store(0, std::memory_order_relaxed);
    cb_trips_.store(0, std::memory_order_relaxed);
    rate_limited_.store(0, std::memory_order_relaxed);
}
