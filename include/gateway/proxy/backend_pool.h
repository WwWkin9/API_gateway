#pragma once

#include "gateway/core/config.h"         // Backend
#include "gateway/proxy/backend.h"       // BackendConnection

#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <ctime>

// 后端连接池：管理到各后端的 TCP 连接
//
// 线程安全：acquire/release/evict 可能从线程池多线程调用
//         cleanup_idle 从主线程调用

class BackendPool {
public:
    BackendPool(size_t max_idle_per_host = 10, int idle_timeout_sec = 60);
    
    BackendPool(const BackendPool&) = delete;
    BackendPool& operator=(const BackendPool&) = delete;
    
    // 获取一个到后端的连接（优先复用空闲连接，否则新建）
    // 返回值可能为 nullptr（连接失败）
    std::shared_ptr<BackendConnection> acquire(const Backend& backend, int connect_timeout_ms);

    // 归还连接到池中（连接健康时调用）
    void release(const Backend& backend, std::shared_ptr<BackendConnection> conn);

    // 驱逐异常连接（调用方判定连接不可用时调用，不会放回池中）
    void evict(const Backend& backend, std::shared_ptr<BackendConnection> conn);

    // 清理所有后端中超过 idle_timeout_sec 的空闲连接
    void cleanup_idle();

    // 预热连接池：为所有后端预先建立连接，减少首次请求延迟
    // backend_count: 后端总数，warm_count: 每个后端预建连接数
    void warm_up(const std::vector<Backend>& backends, int connect_timeout_ms,
                 size_t warm_count);
    
private:
    static std::string make_key(const std::string& host, int port);
    static std::string make_key(const Backend& backend);

    struct PoolEntry {
        std::queue<std::shared_ptr<BackendConnection>> idle;
        size_t active_count = 0; // 当前活动连接数
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, PoolEntry> pools_;
    size_t max_idle_per_host_; // 最大空闲连接数
    int idle_timeout_sec_; // 空闲超时时间，单位秒
};