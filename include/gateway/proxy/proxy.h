#pragma once

#include "gateway/core/config.h"    // Backend
#include "gateway/proxy/backend_pool.h"  // BackendPool

#include <string>
#include <sys/socket.h>
#include <utility>

// 反向代理：将原始 HTTP 请求转发到后端，返回后端响应
//
// 当前实现：每次请求新建 TCP 连接（短连接）
// 后续升级：配合 BackendPool 实现连接复用
class Proxy {
public:
    explicit Proxy(int backend_timeout_ms = 3000);
    ~Proxy() = default;

    void set_pool(std::shared_ptr<BackendPool> pool) { pool_ = std::move(pool); }
    std::shared_ptr<BackendPool> pool() const { return pool_; }
    void cleanup_pool() { if (pool_) pool_->cleanup_idle(); }

    std::string forward(const Backend& backend, const std::string& raw_request) const;

    void set_timeout(int timeout_ms) { backend_timeout_ms_ = timeout_ms; }
    int timeout() const { return backend_timeout_ms_; }

private:
    int backend_timeout_ms_;
    std::shared_ptr<BackendPool> pool_;

    // 直接转发：每次请求新建 TCP 连接（短连接）
    std::string forward_direct(const Backend& backend, const std::string& raw_request) const;
    // 从连接池获取连接，复用连接
    std::string forward_pooled(const Backend& backend, const std::string& raw_request) const;

    static int nb_connect(int fd, const sockaddr* addr, socklen_t len, int timeout_ms);
    static ssize_t nb_send_all(int fd, const char* data, size_t len, int timeout_ms);
    static std::string make_502_response();
};