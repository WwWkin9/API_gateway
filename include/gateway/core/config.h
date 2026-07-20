#pragma once

#include <string>
#include <vector>

// 网关配置结构
struct Backend {
    std::string host;
    int port;
};

struct Route {
    std::string prefix;
    std::vector<Backend> backends;
};

struct GatewayConfig {
    int listen_port = 8080;
    int keep_alive_timeout_sec = 10;   // keep-alive 空闲超时
    int thread_count = 4;              // 线程池大小
    int backend_timeout_ms = 3000;     // 后端转发超时（毫秒）
    int max_request_size = 65536;      // 请求体最大字节数（64 KB）
    int max_epoll_events = 512;        // 每轮 epoll_wait 最大事件数
    int idle_cleanup_interval_sec = 5;   // 空闲连接清理间隔（秒）
    int pool_max_idle_per_host = 10;     // 每个后端最大空闲连接数
    int pool_idle_timeout_sec = 60;      // 连接池空闲连接超时（秒）
    std::vector<Route> routes;
};

//  加载默认配置
GatewayConfig load_default_config();