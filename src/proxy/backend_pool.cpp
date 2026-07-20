#include "gateway/proxy/backend_pool.h"

#include <iostream>
#include <sstream>

// ============== 工具：生成 key ==============

std::string BackendPool::make_key(const std::string& host, int port) {
    std::ostringstream oss;
    oss << host << ':' << port;
    return oss.str();
}

std::string BackendPool::make_key(const Backend& backend) {
    return make_key(backend.host, backend.port);
}

// ============== 构造 ==============

BackendPool::BackendPool(size_t max_idle_per_host, int idle_timeout_sec)
    : max_idle_per_host_(max_idle_per_host)
    , idle_timeout_sec_(idle_timeout_sec) 
{}

// ============== 获取连接 ==============

std::shared_ptr<BackendConnection> BackendPool::acquire(const Backend& backend, int connect_timeout_ms) {
    std::string key = make_key(backend);

    std::lock_guard<std::mutex> lock(mutex_);

    auto& entry = pools_[key];
    entry.active_count++;

    // 1. 尝试从空闲队列取一个健康连接
    while (!entry.idle.empty()) {
        auto conn = entry.idle.front();
        entry.idle.pop();
        
        if (conn->is_alive()) {
            conn->touch();
            return conn;
        }
        // 不健康的连接直接丢弃
    }
    // 2. 空闲队列为空，新建连接
    auto conn = std::make_shared<BackendConnection>(backend.host, backend.port);
    if (conn->connect(connect_timeout_ms)) {
        return conn;
    }
    // 3. 连接失败，返回 nullptr
    entry.active_count--;
    return nullptr;
}

// ============== 归还连接 ==============

void BackendPool::release(const Backend& backend, std::shared_ptr<BackendConnection> conn) {

    if (!conn || !conn->is_alive()) {
        evict(backend, conn);
        return;
    }
    
    std::string key = make_key(backend);

    std::lock_guard<std::mutex> lock(mutex_);

    auto& entry = pools_[key];
    if (entry.active_count > 0) entry.active_count--;

    // 检查空闲队列是否已满
    if (entry.idle.size() < max_idle_per_host_) {
        entry.idle.push(std::move(conn));
    }
    // 超过上限则丢弃（conn 的 shared_ptr 出作用域后自动析构）
}

// ============== 驱逐 ==============

void BackendPool::evict(const Backend& backend, std::shared_ptr<BackendConnection> conn) {
    if (!conn) return;

    (void)backend;  // 驱逐不需要按 key 查找，直接析构即可

    std::lock_guard<std::mutex> lock(mutex_);

    // 更新 active_count
    std::string key = make_key(backend);
    auto it = pools_.find(key);
    if (it != pools_.end() && it->second.active_count > 0) {
        it->second.active_count--;
    }

    // conn 析构时自动 close
}
        
// ============== 清理空闲 ==============

void BackendPool::cleanup_idle() {
    std::time_t now = std::time(nullptr);

    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [key, entry] : pools_) {
        size_t n = entry.idle.size();
        for (size_t i = 0; i < n; ++i) {
            auto& conn = entry.idle.front();

            if (now - conn->last_activity() >= idle_timeout_sec_) {
                entry.idle.pop();  // 超时，丢弃
            } else {
                // 未超时且遍历到第一个未超时的，后面的也不会超时（FIFO）
                // 但为了安全，仍然逐个检查
                entry.idle.pop();
                entry.idle.push(std::move(conn));
            }
        }
    }
}

// ============== 统计 ==============

size_t BackendPool::idle_count(const Backend& backend) const {
    std::string key = make_key(backend);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pools_.find(key);
    return it != pools_.end() ? it->second.idle.size() : 0;
}

size_t BackendPool::total_idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto& [key, entry] : pools_) {
        total += entry.idle.size();
    }
    return total;
}