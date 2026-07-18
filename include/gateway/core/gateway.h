#pragma once

#include "gateway/core/config.h"
#include "gateway/filter/filter.h"
#include "gateway/utils/thread_pool.h"

#include <memory>
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
    std::unordered_map<int, ConnState> conn_states_;
    std::vector<std::unique_ptr<Filter>> filters_;
    std::unique_ptr<ThreadPool> pool_;
    std::time_t last_cleanup_time_ = 0;

    // 线程池 worker：对已读取的 raw 进行解析、过滤、转发
    void process_request(int fd, std::string raw);

    // reactor 回调：读请求并派发给线程池
    void handle_client(int epfd, int fd);
    void cleanup_idle_connections(int epfd);

    Backend route(const std::string& path) const;
    std::string forward_to_backend(const Backend& backend, const std::string& raw_request) const;
};