#pragma once

#include "gateway/core/config.h"   // Backend

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// 负载均衡算法
enum class LBAlgorithm {
    RoundRobin,       // 轮询（默认）
    Random,           // 随机
    LeastConnections  // 最小连接数
};

// 负载均衡器：从一组后端中选择一个
//
// 线程安全：select 可能从线程池多线程调用
class LoadBalancer {
public:
    LoadBalancer(LBAlgorithm algo = LBAlgorithm::RoundRobin);

    // 从后端列表中选择一个
    // backends 为空时返回空 Backend（host 为空）
    Backend select(const std::vector<Backend>& backends);

    // 连接数跟踪（LeastConnections 模式需要）
    void on_connect(const Backend& backend);
    void on_disconnect(const Backend& backend);
private:
    LBAlgorithm algo_;
    std::atomic<size_t> rr_counter_{0};

    mutable std::mutex mutex_;
    std::unordered_map<std::string, size_t> conn_counts_;

    static std::string make_key(const std::string& host, int port);
    static std::string make_key(const Backend& backend);

    Backend select_roundRobin(const std::vector<Backend>& backends);
    Backend select_random(const std::vector<Backend>& backends);
    Backend select_leastConnections(const std::vector<Backend>& backends);

};