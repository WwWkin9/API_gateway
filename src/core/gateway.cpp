#include "gateway/core/gateway.h"

#include "gateway/http/request.h"
#include "gateway/http/response.h"
#include "gateway/utils/utils.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <ctime>
#include <iostream>
#include <string.h>



Gateway::Gateway(const GatewayConfig& config) : config_(config) {}

void Gateway::add_filter(std::unique_ptr<Filter> filter) {
    filters_.push_back(std::move(filter));
}

Backend Gateway::route(const std::string& path) const {
    for (const auto& r : config_.routes) {
        if (path.rfind(r.prefix, 0) == 0) {
            return r.backend;
        }
    }
    return {"", 0};
}

// 非阻塞 connect，返回 0 成功，-1 失败/超时
static int nb_connect(int fd, const sockaddr* addr, socklen_t len, int timeout_ms) {
    set_nonblocking(fd);

    int ret = connect(fd, addr, len);
    if (ret == 0) return 0;  // 本地连接可能立即成功
    if (errno != EINPROGRESS) return -1;

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return -1;  // 超时或错误

    // 检查连接是否真正成功
    int err = 0;
    socklen_t errlen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0)
        return -1;
    return 0;
}

// 非阻塞发送全部数据，带超时
static ssize_t nb_send_all(int fd, const char* data, size_t len, int timeout_ms) {
    size_t sent = 0;
    while (sent < len) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) return -1;

        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        } else {
            return -1;
        }
    }
    return (ssize_t)sent;
}

// 反向代理到后端（非阻塞 IO + 超时）
std::string Gateway::forward_to_backend(const Backend& backend, const std::string& raw_request) const {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return make_502(false).to_string();

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(backend.port);
    if (inet_pton(AF_INET, backend.host.c_str(), &addr.sin_addr) <= 0) {
        close(fd);
        return make_502(false).to_string();
    }

    int tmo = config_.backend_timeout_ms;

    if (nb_connect(fd, (sockaddr*)&addr, sizeof(addr), tmo) < 0) {
        close(fd);
        return make_502(false).to_string();
    }

    if (nb_send_all(fd, raw_request.data(), raw_request.size(), tmo) < 0) {
        close(fd);
        return make_502(false).to_string();
    }

    // 确保 socket 为非阻塞（nb_connect 已设置，这里保险）
    (void)set_nonblocking(fd);

    std::string resp;
    char buf[4096];
    bool got_data = false;
    while (true) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, got_data ? (tmo / 2) : tmo);
        if (ret < 0) break;
        if (ret == 0) break;   // 超时

        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            resp.append(buf, (size_t)n);
            got_data = true;
        } else if (n == 0) {
            break;  // 对端关闭
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
    }

    close(fd);
    return resp.empty() ? make_502(false).to_string() : resp;
}

void Gateway::cleanup_idle_connections() {
    std::time_t now = std::time(nullptr);
    std::time_t timeout = config_.keep_alive_timeout_sec;
    
    auto it = connections_.begin();
    while (it != connections_.end()) {
        auto conn = it->second;
        if (now - conn->last_active_time() >= timeout && conn->state() == Connection::State::Reading) {
            // 只清理处于 Reading 状态的空闲连接（不干扰正在处理/写响应的连接）
            conn->force_close();
             // force_close → close_internal → close_cb → connections_.erase(fd)
            // 所以 erase 已在回调中完成，这里直接重新开始遍历
            it = connections_.begin();
        } else {
            ++it;
        }
    }
}


void Gateway::process_request(int fd, std::string raw) {
    HttpRequest req;
    if (!parse_http_request(raw, req)) {
        auto resp = make_400(false).to_string();
        send(fd, resp.c_str(), resp.size(), 0);
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            it->second->force_close();
        }
        return;
    }

    // ---- 过滤器链：请求阶段 ----
    FilterContext fctx;
    fctx.req = &req;
    for (auto& f : filters_) {
        if (!f->on_request(fctx)) {
            auto resp = make_400(false).to_string();
            send(fd, resp.c_str(), resp.size(), 0);

            auto it = connections_.find(fd);
            if (it != connections_.end()) {
                it->second->force_close();
            }
            return;
        }
    }

    // ---- 路由 + 转发 ----
    Backend backend = route(req.path);

    std::string out;
    if (backend.host.empty()) {
        out = make_404(false).to_string();
    } else {
        out = forward_to_backend(backend, raw);
    }

    // 如果后端没写 Connection 头，简单补上
    if (out.find("Connection:") == std::string::npos) {
        size_t pos = out.find("\r\n\r\n");
        if (pos != std::string::npos) {
            std::string head = out.substr(0, pos);
            std::string body = out.substr(pos + 4);
            out = head + "\r\nConnection: close\r\n\r\n" + body;
        }
    }

    // ---- 过滤器链：响应阶段 ----
    {
        HttpResponse tmp_resp;
        // 安全解析后端返回的状态码
        size_t sp1 = out.find(' ');
        if (sp1 != std::string::npos) {
            size_t sp2 = out.find(' ', sp1 + 1);
            if (sp2 != std::string::npos) {
                auto parsed = parse_int_safe(out.c_str() + sp1 + 1, out.c_str() + sp2);
                if (parsed.has_value()) {
                    tmp_resp.status_code = parsed.value();
                }
            }
        }
        fctx.resp = &tmp_resp;
        for (auto& f : filters_) {
            f->on_response(fctx);
        }

        // 优化：一次性找到 \r\n\r\n 分隔点，收集新增头，在分隔点前 insert
        if (!tmp_resp.headers.empty()) {
            size_t hdr_end = out.find("\r\n\r\n");
            if (hdr_end != std::string::npos) {
                std::string extra;
                for (const auto& [k, v] : tmp_resp.headers) {
                    // 只在响应中没有该头时才追加
                    std::string tag = "\r\n" + k + ":";
                    if (out.find(tag) == std::string::npos) {
                        extra += "\r\n";
                        extra += k;
                        extra += ": ";
                        extra += v;
                    }
                }
                if (!extra.empty()) {
                    out.insert(hdr_end, extra);
                }
            }
        }
    }

    // ---- 通过 Connection 发送 ----
    auto conn = connections_.find(fd);
    if (conn != connections_.end()) {
        // 判断是否 keep-alive 连接
        bool ka = request_keep_alive(req);
        conn->second->send(out, ka);
    } else {
        // 连接已不在（不应发生），直接写 + 关闭
        send(fd, out.c_str(), out.size(), 0);
        close(fd);
    }
}

void Gateway::run() {
    // ---- 1. 初始化事件循环 ----
    try {
        event_loop_ = std::make_unique<EventLoop>(config_.max_epoll_events);
    } catch (const std::exception& e) {
        std::cerr << "Gateway::run exception: " << e.what() << std::endl;
        return;
    }
   
    // ---- 2. 创建线程池 ----
    pool_ = std::make_unique<ThreadPool>(config_.thread_count);

    // ---- 3. 注册 listen_fd 的回调: accept 接受新连接 ----
    tcp_server_ = std::make_unique<TCPServer>(event_loop_.get(), config_.listen_port);
    tcp_server_->set_new_connection_callback([this](int cfd, std::string peer_ip, int peer_port){
        (void)peer_ip;
        (void)peer_port;
        auto conn = std::make_shared<Connection>(
               event_loop_.get(), cfd, config_.max_request_size
        );
      
        conn->set_message_callback([this](int fd, std::string raw) {
            pool_->submit([this, fd, raw = std::move(raw)]() {
                process_request(fd, std::move(raw));
            });
        });
        conn->set_close_callback([this](int fd) {
            connections_.erase(fd);
        });
        event_loop_->add_fd(cfd, EPOLLIN | EPOLLOUT);
        event_loop_->set_callback(cfd, [this, conn](int /*fd*/, uint32_t events) mutable{
           if (events & EPOLLIN) {
               conn->on_read();
           }
           if (events & EPOLLOUT) {
               conn->on_write();
           }
           if (events & (EPOLLHUP | EPOLLERR)) {
               conn->force_close();
           }
        });
        connections_[cfd] = conn;
    });
    tcp_server_->start();

    // ---- 4. 注册空闲回调 ----
    event_loop_->set_idle_callback([this](){
        std::time_t now = std::time(nullptr);
        if (now - last_cleanup_time_ >= config_.idle_cleanup_interval_sec) {
            cleanup_idle_connections();
            last_cleanup_time_ = now;
        }
    });
    std::cout << "Gateway listening on port " << config_.listen_port
              << " (reactor + " << config_.thread_count << " workers)\n";

    // ---- 5. 启动事件循环（阻塞直到 stop() 被调用）
    event_loop_->run(1000);
}