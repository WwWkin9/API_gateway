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


#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>
#include <ctime>

// 连接状态
struct ConnState {
    std::time_t last_active;
};

class Gateway {
public:
    explicit Gateway(const GatewayConfig& config);
    void run();

    // 添加过滤器（按添加顺序执行）
    void add_filter(std::unique_ptr<Filter> filter);

private:
    GatewayConfig config_;
    std::unique_ptr<EventLoop> event_loop_;
    std::unique_ptr<ThreadPool> pool_;

    // TCP 服务器
    std::unique_ptr<TCPServer> tcp_server_;

    // 连接管理（shared_ptr：被 EventLoop 的 lambda 捕获）
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;

    // 路由表
    std::unique_ptr<Router> router_;

    // 代理
    std::unique_ptr<Proxy> proxy_;

    // 负载均衡器
    std::unique_ptr<LoadBalancer> load_balancer_;

    // 断路器（按后端 "host:port" 索引）
    std::unordered_map<std::string, CircuitBreaker> circuit_breakers_;
    mutable std::mutex cb_mutex_;

    // 过滤器链
    std::vector<std::unique_ptr<Filter>> filters_;
    std::time_t last_cleanup_time_ = 0;

    // 线程池 worker：对已读取的 raw 进行解析、过滤、转发
    void process_request(int fd, std::string raw);

    void cleanup_idle_connections();

    // 获取或创建指定后端的断路器
    CircuitBreaker& get_circuit_breaker(const Backend& backend);

    // 发送错误响应并关闭连接
    void send_error_and_close(int fd, const std::string& resp);
};