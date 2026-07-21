#include "gateway/core/config.h"

// 默认路由配置
GatewayConfig load_default_config() {
    GatewayConfig cfg;
    cfg.listen_port = 8080;
    cfg.keep_alive_timeout_sec = 10;
    cfg.thread_count = 4;
    cfg.backend_timeout_ms = 3000;
    cfg.max_request_size = 65536;
    cfg.pool_max_idle_per_host = 10;
    cfg.pool_idle_timeout_sec = 60;
    cfg.max_epoll_events = 512;
    cfg.idle_cleanup_interval_sec = 5;
    cfg.max_connections = 10000;
    cfg.max_queue_size = 2048;
    cfg.max_deferred_per_round = 256;

    cfg.routes.push_back({"/api/user", {{"127.0.0.1", 9001}}});
    cfg.routes.push_back({"/api/order", {{"127.0.0.1", 9002}}});

    return cfg;
}