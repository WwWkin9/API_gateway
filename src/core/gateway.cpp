#include "gateway/core/gateway.h"

#include "gateway/http/request.h"
#include "gateway/http/response.h"
#include "gateway/proxy/backend_pool.h"
#include "gateway/utils/utils.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <ctime>
#include <iostream>
#include <string.h>



Gateway::Gateway(const GatewayConfig& config) : config_(config) {
    router_ = std::make_unique<Router>(config.routes);
    proxy_ = std::make_unique<Proxy>(config.backend_timeout_ms);

    // 创建后端连接池并注入 Proxy
    auto pool = std::make_shared<BackendPool>(config.pool_max_idle_per_host,
                                              config.pool_idle_timeout_sec);
    proxy_->set_pool(std::move(pool));
}

void Gateway::add_filter(std::unique_ptr<Filter> filter) {
    filters_.push_back(std::move(filter));
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
    std::string out;
    auto backend_opt = router_->match(req.path);
    if (!backend_opt.has_value()) {
        out = make_404(false).to_string();
    } else {
        out = proxy_->forward(backend_opt.value(), raw);
    }
    
    if (backend_opt.value().host.empty()) {
        out = make_404(false).to_string();
    } else {
        out = proxy_->forward(backend_opt.value(), raw);
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

void Gateway::cleanup_idle_connections() {
    std::time_t now = std::time(nullptr);
    auto it = connections_.begin();
    while (it != connections_.end()) {
        if (now - it->second->last_active_time() > config_.keep_alive_timeout_sec) {
            it->second->force_close();
            it = connections_.erase(it);
        } else {
            ++it;
        }
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
            proxy_->cleanup_pool();
            last_cleanup_time_ = now;
        }
    });
    std::cout << "Gateway listening on port " << config_.listen_port
              << " (reactor + " << config_.thread_count << " workers)\n";

    // ---- 5. 启动事件循环（阻塞直到 stop() 被调用）
    event_loop_->run(1000);
}