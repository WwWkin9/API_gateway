#include "gateway/proxy/backend_pool.h"

#include <cstdio>

// ============== 工具：生成 key ==============

std::string BackendPool::make_key(const std::string& host, int port) {
    std::string key;
    key.reserve(host.size() + 12);
    key += host;
    key += ':';
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", port);
    key += port_buf;
    return key;
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

    // 1. 尝试从空闲队列取一个健康连接（加锁）
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto& entry = pools_[key];
        entry.active_count++;

        while (!entry.idle.empty()) {
            auto conn = entry.idle.front();
            entry.idle.pop();
            
            if (conn->is_alive()) {
                conn->touch();
                return conn;
            }
            // 不健康的连接直接丢弃
        }
    }
    // 2. 空闲队列为空，新建连接（不加锁，避免阻塞其他线程）
    auto conn = std::make_shared<BackendConnection>(backend.host, backend.port);
    if (conn->connect(connect_timeout_ms)) {
        return conn;
    }
    // 3. 连接失败，回滚 active_count
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pools_.find(key);
        if (it != pools_.end() && it->second.active_count > 0) {
            it->second.active_count--;
        }
    }
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
            // 必须拷贝 shared_ptr，因为 pop() 会销毁 deque 元素
            // 若引用计数刚好为 1（仅队列持有），pop 会释放 BackendConnection
            // 后续 move 会读取已释放内存导致 use-after-free
            auto conn = entry.idle.front();

            if (now - conn->last_activity() >= idle_timeout_sec_) {
                entry.idle.pop();  // 超时，丢弃（conn 析构时释放）
            } else {
                entry.idle.pop();
                entry.idle.push(std::move(conn));
            }
        }
    }
}

// ============== 预热 ==============

void BackendPool::warm_up(const std::vector<Backend>& backends, int connect_timeout_ms,
                          size_t warm_count) {
    if (backends.empty() || warm_count == 0) return;
    
    fprintf(stderr, "[pool] warming up %zu backends with %zu connections each...\n",
            backends.size(), warm_count);
    
    for (const auto& backend : backends) {
        for (size_t i = 0; i < warm_count; ++i) {
            auto conn = std::make_shared<BackendConnection>(backend.host, backend.port);
            if (conn->connect(connect_timeout_ms)) {
                release(backend, std::move(conn));
            }
        }
    }
    
    fprintf(stderr, "[pool] warm_up done\n");
}

