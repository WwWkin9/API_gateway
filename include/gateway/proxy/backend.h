#pragma once

#include <string>
#include <ctime>

// 单条到后端服务的 TCP 连接
// 生命周期由 BackendPool 通过 shared_ptr 管理
class BackendConnection {
public:
    BackendConnection(const std::string& host, int port);
    ~BackendConnection();

    // 禁用复制构造函数和赋值运算符
    BackendConnection(const BackendConnection&) = delete;
    BackendConnection& operator=(const BackendConnection&) = delete;

    // ---- 连接 ----
    // 建立 TCP 连接，失败返回 false
    bool connect(int timeout_ms);

    // ---- IO ----
    // 发送全部数据（带超时），失败返回 false
    bool send_all(const char* data, size_t len, int timeout_ms);

    // 读取直到对端关闭或超时，返回读取到的全部数据
    // 返回空字符串表示读取失败
    std::string recv_all(int timeout_ms);

    // ---- 状态 ----
    // 连接是否存活（SO_ERROR 检查）
    bool is_alive() const;

    int fd() const { return fd_; }
    const std::string& host() const { return host_; }
    int port() const { return port_; }

    // 最后使用时间
    std::time_t last_activity() const { return last_activity_; }
    void touch() { last_activity_ = std::time(nullptr); }

    // 强制关闭
    void close();

private:
    int fd_ = -1;
    std::string host_;
    int port_;
    std::time_t last_activity_ = 0;
};