#pragma once

#include "gateway/core/config.h"
#include "gateway/filter/filter.h"
#include "gateway/utils/thread_pool.h"
#include "gateway/net/event_loop.h"
#include "gateway/net/connection.h"
#include "gateway/net/tcp_server.h"
#include "gateway/core/router.h"
#include "gateway/proxy/proxy.h"
#include "gateway/proxy/load_balancer.h"
#include "gateway/proxy/circuit_breaker.h"
#include "gateway/proxy/health_checker.h"
#include "gateway/filter/rate_limit_filter.h"
#include "gateway/timer/timer.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>
#include <ctime>

class Gateway {
public:
    explicit Gateway(const GatewayConfig& config);
    void run();

    // 添加过滤器（按添加顺序执行）
    void add_filter(std::unique_ptr<Filter> filter);

private:
    GatewayConfig config_;
    std::unique_ptr<EventLoop> event_loop_;
    std::unique_ptr<Timer> timer_;
    std::unique_ptr<ThreadPool> pool_;
    RateLimitFilter* rate_limit_filter_ = nullptr;

    // TCP 服务器
    std::unique_ptr<TCPServer> tcp_server_;

    // 连接管理（shared_ptr：被 EventLoop 的 lambda 捕获）
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    mutable std::mutex connections_mutex_;
    std::atomic<int> connection_count_{0};  // 当前连接数（无锁快速读取）
    int max_connections_ = 0;

    // 路由表
    std::unique_ptr<Router> router_;

    // 代理
    std::unique_ptr<Proxy> proxy_;

    // 负载均衡器
    std::unique_ptr<LoadBalancer> load_balancer_;

    // 断路器（按后端 "host:port" 索引）
    std::unordered_map<std::string, CircuitBreaker> circuit_breakers_;
    mutable std::mutex cb_mutex_;

    // 健康检查器
    std::unique_ptr<HealthChecker> health_checker_;

    // 过滤器链
    std::vector<std::unique_ptr<Filter>> filters_;

    // 线程池 worker：对已读取的 raw 进行解析、过滤、转发
    // 通过 shared_ptr 持有 Connection，防止 fd 复用导致的 use-after-close
    void process_request(std::shared_ptr<Connection> conn, std::string raw);

    void cleanup_idle_connections();

    // 获取或创建指定后端的断路器
    CircuitBreaker& get_circuit_breaker(const Backend& backend);

    // 发送错误响应并关闭连接
    void send_error_and_close(std::shared_ptr<Connection> conn, const std::string& resp);

    // 内部端点（/stats, /metrics, /health）
    bool handle_internal_endpoint(const std::string& path, std::string& response);

    // 收集所有后端（去重）
    std::vector<Backend> collect_all_backends() const;
};