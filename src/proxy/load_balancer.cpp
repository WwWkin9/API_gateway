#include "gateway/proxy/load_balancer.h"


#include <random>
#include <thread>

// ============== 工具 ==============

std::string LoadBalancer::make_key(const std::string& host, int port) {
    std::string key;
    key.reserve(host.size() + 12);
    key += host;
    key += ':';
    key += std::to_string(port);
    return key;
}

std::string LoadBalancer::make_key(const Backend& backend) {
    return make_key(backend.host, backend.port);
}

// ============== 构造 ==============

LoadBalancer::LoadBalancer(LBAlgorithm algo) : algo_(algo) {}

// ============== 对外接口 ==============

Backend LoadBalancer::select(const std::vector<Backend>& backends) {
    if (backends.empty()) return {"", 0};
    if (backends.size() == 1) return backends[0];

    switch (algo_) {
        case LBAlgorithm::RoundRobin:
            return select_roundRobin(backends);
        case LBAlgorithm::Random:
            return select_random(backends);
        case LBAlgorithm::LeastConnections:
            return select_leastConnections(backends);
        default:
            return backends[0];
    }
}

void LoadBalancer::on_connect(const Backend& backend) {
    std::lock_guard<std::mutex> lock(mutex_);
    conn_counts_[make_key(backend)]++;
}

void LoadBalancer::on_disconnect(const Backend& backend) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& count = conn_counts_[make_key(backend)];
    if (count > 0) count--;
}

// ============== 轮询 ==============

Backend LoadBalancer::select_roundRobin(const std::vector<Backend>& backends){
    size_t idx = rr_counter_.fetch_add(1, std::memory_order_relaxed) % backends.size();
    return backends[idx];
}

// ============== 随机 ==============

Backend LoadBalancer::select_random(const std::vector<Backend>& backends) {
    // 线程本地随机数生成器
    static thread_local std::mt19937 rng(
        std::hash<std::thread::id>{}(std::this_thread::get_id()) ^
        static_cast<unsigned>(std::random_device{}()));

    std::uniform_int_distribution<size_t> dist(0, backends.size() - 1);
    return backends[dist(rng)];
}

// ============== 最小连接数 ==============

Backend LoadBalancer::select_leastConnections(const std::vector<Backend>& backends) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 找到连接数最小的后端
    const Backend* best = &backends[0];
    int min_conn = conn_counts_[make_key(*best)];

    for (size_t i = 1; i < backends.size(); ++i) {
        int count = conn_counts_[make_key(backends[i])];
        if (count < min_conn) {
            min_conn = count;
            best = &backends[i];
        }
    }

    conn_counts_[make_key(*best)]++;  // 预增加（比 select 后调 on_connect 更简单）

    return *best;
}
