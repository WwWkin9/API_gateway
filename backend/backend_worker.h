#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// 简单线程池：accept 线程将 fd 入队，worker 线程取 fd 处理
class BackendWorkerPool {
public:
    using Handler = std::function<void(int fd)>;

    BackendWorkerPool(int num_workers, Handler handler)
        : handler_(std::move(handler)), stop_(false) {
        for (int i = 0; i < num_workers; ++i) {
            workers_.emplace_back([this]() { run(); });
        }
    }

    ~BackendWorkerPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    void submit(int fd) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(fd);
        }
        cv_.notify_one();
    }

    size_t queue_size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    void run() {
        while (true) {
            int fd = -1;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                fd = queue_.front();
                queue_.pop();
            }
            handler_(fd);
        }
    }

    std::vector<std::thread> workers_;
    std::queue<int> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    Handler handler_;
    bool stop_;
};

// keep-alive 模式处理客户端：循环读取 HTTP 请求，每次返回响应后继续等待下一个请求
// 客户端关闭连接（recv==0）或 Connection: close 时退出
inline void handle_client_keepalive(int cfd, const std::string& service_name) {
    std::string req_buf;
    req_buf.reserve(4096);
    char buf[4096];

    while (true) {
        req_buf.clear();

        // 读取请求：循环 recv 直到遇到 \r\n\r\n 或 EOF
        while (true) {
            ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n > 0) {
                req_buf.append(buf, static_cast<size_t>(n));
                if (req_buf.find("\r\n\r\n") != std::string::npos) break;
            } else if (n == 0) {
                // 客户端关闭
                ::close(cfd);
                return;
            } else {
                // 错误
                fprintf(stderr, "[%s] recv error on fd=%d: %s\n", service_name.c_str(), cfd, strerror(errno));
                ::close(cfd);
                return;
            }
        }

        // 解析 body
        auto pos = req_buf.find("\r\n\r\n");
        std::string body = (pos != std::string::npos) ? req_buf.substr(pos + 4) : "";

        // 检查客户端是否要关闭
        bool client_close = (req_buf.find("\r\nConnection: close") != std::string::npos ||
                             req_buf.find("\r\nconnection: close") != std::string::npos);

        std::string resp_body = "{\"service\":\"" + service_name + "\",\"body\":\"" + body + "\"}";
        std::string conn_header = client_close ? "Connection: close" : "Connection: keep-alive";
        std::string resp =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" +
            conn_header + "\r\nContent-Length: " +
            std::to_string(resp_body.size()) + "\r\n\r\n" + resp_body;

        ::send(cfd, resp.c_str(), resp.size(), MSG_NOSIGNAL);

        if (client_close) {
            ::close(cfd);
            return;
        }
        // keep-alive：继续循环等待下一个请求
    }
}
