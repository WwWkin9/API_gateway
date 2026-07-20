#pragma once

#include "gateway/net/buffer.h"
#include "gateway/net/event_loop.h"
#include "gateway/http/parser.h"

#include <functional>
#include <string>
#include <ctime>

// 连接：封装客户端连接的读写操作、HTTP 消息边界识别、生命周期管理
//
// 状态机：
//   Reading ──(收到完整消息)──> Processing ──(send 被调用)──> Writing
//                                                                │
//                                           ┌─────────────────────┘
//                                           │ (keep-alive → 回到 Reading)
//                                           │ (!keep-alive → Closed)
//                                           └─────────────────────┘
//
// 最大请求大小：64KB
const int kMaxRequestSize = 65536;

class Connection {
public:
    enum class State {
        Reading,
        Processing,
        Writing,
        Closed,
    };

    // 收到完整消息的回调（fd + 原始请求数据）
    using MessageCallback = std::function<void(int fd, std::string raw)>;
    // 连接关闭的回调
    using CloseCallback = std::function<void(int fd)>;

    Connection(EventLoop* event_loop, int fd, int max_request_size = kMaxRequestSize);
    ~Connection();
        
    // 禁用复制构造函数和赋值运算符
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // ---- 属性 ----
    int fd() const { return fd_; }
    State state() const { return state_; }
    std::time_t last_active_time() const { return last_active_time_; }
    int max_request_size() const { return max_request_size_; }

    // ---- 回调注册 ----
    void set_message_callback(MessageCallback cb);
    void set_close_callback(CloseCallback cb);

    // ---- 事件处理（由 EventLoop 回调触发） ----
    void on_read();   // fd 可读时调用
    void on_write();  // fd 可写时调用

    // ---- 响应处理 ----
    void finish_response(); // 完成响应，切换状态为 Writing

    // ---- 发送响应（由上层在处理完请求后调用） ----
    // keep_alive: 响应完成后是否继续保持连接等待下次请求
    void send(const std::string& data, bool keep_alive = false);
    
    
    // ---- 强制关闭 ----
    void force_close();
private:
    EventLoop* event_loop_;
    int fd_;
    State state_;
    std::time_t last_active_time_;
    int max_request_size_;

    Buffer read_buf_;
    Buffer write_buf_;

    MessageCallback message_callback_;
    CloseCallback close_callback_;

    std::atomic<bool> keep_alive_{false};

    // 尝试从 read_buf_ 中增量解析 HTTP 消息
    // 当解析出一条完整消息时回调 message_callback_
    void try_parse_message();

    HttpParser parser_;

    void close_internal();
};