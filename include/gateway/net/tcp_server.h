#pragma once

#include "gateway/net/event_loop.h"

#include <functional>
#include <string>

// TCP 服务器：管理监听 socket，accept 新连接并通过回调通知上层
class TCPServer {
public:
    // 新连接回调：(fd, 对端 IP, 对端端口)
    using NewConnectionCallback = std::function<void(int fd, const std::string& peer_ip, int peer_port)>;

    TCPServer(EventLoop* event_loop, int port);
    ~TCPServer();

    TCPServer(const TCPServer&) = delete;
    TCPServer& operator=(const TCPServer&) = delete;
    
    void set_new_connection_callback(NewConnectionCallback cb);

    // 启动服务器：监听端口并开始 accept 新连接
    void start();
    
    int listen_fd() const { return listen_fd_; }
    int port() const { return port_; }
private:
    EventLoop* event_loop_;
    int listen_fd_ = -1;
    int port_;
    NewConnectionCallback new_connection_cb_;

    void on_accept();
};