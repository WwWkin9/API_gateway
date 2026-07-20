#include "gateway/core/gateway.h"

#include "gateway/http/request.h"
#include "gateway/http/response.h"
#include "gateway/logger/logger.h"
#include "gateway/proxy/backend_pool.h"
#include "gateway/utils/utils.h"
#include "gateway/filter/rate_limit_filter.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <ctime>
#include <sstream>
#include <string.h>



Gateway::Gateway(const GatewayConfig& config) : config_(config) {
    router_ = std::make_unique<Router>(config.routes);
    proxy_ = std::make_unique<Proxy>(config.backend_timeout_ms);
    load_balancer_ = std::make_unique<LoadBalancer>(LBAlgorithm::RoundRobin);

    // 创建后端连接池并注入 Proxy
    auto pool = std::make_shared<BackendPool>(config.pool_max_idle_per_host,
                                              config.pool_idle_timeout_sec);
    proxy_->set_pool(std::move(pool));
}

void Gateway::add_filter(std::unique_ptr<Filter> filter) {
    filters_.push_back(std::move(filter));
}

void Gateway::process_request(int fd, std::string raw) {
    // 1. 解析请求
    HttpRequest req;
    if (!parse_http_request(raw, req)) {
        send_error_and_close(fd, make_400(false).to_string());
        return;
    }

    // 2. 过滤器链：请求阶段
    FilterContext fctx;
    fctx.req = &req;
    for (auto& f : filters_) {
        if (!f->on_request(fctx)) {
            send_error_and_close(fd, make_400(false).to_string());
            return;
        }
    }

    // 3. 路由 + 负载均衡
    auto backends = router_->match(req.path);
    if (!backends.has_value()) {
        send_error_and_close(fd, make_404(false).to_string());
        return;
    }
    Backend target = load_balancer_->select(backends.value());

    // 4. 断路器检查
    auto& cb = get_circuit_breaker(target);
    if (!cb.allow_request()) {
        // 断路器开路，快速失败
        std::string resp503 =
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Content-Length: 19\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Service Unavailable";
        send_error_and_close(fd, resp503);
        return;
    }

    // 5. 转发到后端
    load_balancer_->on_connect(target);
    std::string raw_resp = proxy_->forward(target, raw);
    load_balancer_->on_disconnect(target);

    // 6. 断路器结果上报
    bool success = !raw_resp.empty() && raw_resp.find("HTTP/1.1 5") != 0;
    if (success) {
        cb.on_success();
    } else {
        cb.on_failure();
    }

    std::string out = raw_resp;

    // 7. 如果后端没写 Connection 头，简单补上
    if (out.find("Connection:") == std::string::npos) {
        size_t pos = out.find("\r\n\r\n");
        if (pos != std::string::npos) {
            std::string head = out.substr(0, pos);
            std::string body = out.substr(pos + 4);
            out = head + "\r\nConnection: close\r\n\r\n" + body;
        }
    }

    // 8. 过滤器链：响应阶段
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

        // 一次性找到 \r\n\r\n 分隔点，收集新增头，在分隔点前 insert
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

    // 9. 通过 Connection 发送
    auto conn = connections_.find(fd);
    if (conn != connections_.end()) {
        bool ka = request_keep_alive(req);
        conn->second->send(out, ka);
    } else {
        // 连接已不在（不应发生），直接写 + 关闭
        send(fd, out.c_str(), out.size(), 0);
        close(fd);
    }
}

CircuitBreaker& Gateway::get_circuit_breaker(const Backend& backend) {
    std::ostringstream oss;
    oss << backend.host << ':' << backend.port;
    std::string key = oss.str();

    // 快速路径：不加锁先查找
    {
        auto it = circuit_breakers_.find(key);
        if (it != circuit_breakers_.end()) {
            return it->second;
        }
    }

    // 慢速路径：加锁创建（try_emplace 就地构造，避免拷贝/移动）
    std::lock_guard<std::mutex> lock(cb_mutex_);
    return circuit_breakers_.try_emplace(
        key, 5, config_.backend_timeout_ms * 2, 3
    ).first->second;
}

void Gateway::send_error_and_close(int fd, const std::string& resp) {
    auto it = connections_.find(fd);
    if (it != connections_.end()) {
        it->second->send(resp, false);  // keep_alive = false，发送后关闭
    } else {
        ::send(fd, resp.c_str(), resp.size(), 0);
        ::close(fd);
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
        LOG_ERROR("Gateway::run EventLoop init failed: %s", e.what());
        return;
    }

    // ---- 2. 初始化定时器并关联到 EventLoop ----
    timer_ = std::make_unique<Timer>();
    event_loop_->set_timer(timer_.get());

    // ---- 3. 创建线程池 ----
    pool_ = std::make_unique<ThreadPool>(config_.thread_count);

    // ---- 4. 注册 listen_fd 的回调: accept 接受新连接 ----
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

    // ---- 5. 注册周期性定时任务 ----
    // 空闲连接清理（每隔 idle_cleanup_interval_sec 秒执行）
    int cleanup_ms = config_.idle_cleanup_interval_sec * 1000;
    timer_->run_every(cleanup_ms, [this]() {
        cleanup_idle_connections();
        proxy_->cleanup_pool();

        // 清理限流器中 5 分钟未活跃的桶
        if (rate_limit_filter_) {
            rate_limit_filter_->cleanup_expired_buckets(300);
        }
    });

    // ---- 6. 注册空闲回调：驱动定时器 tick ----
    event_loop_->set_idle_callback([this]() {
        timer_->tick();
    });

    LOG_INFO("Gateway listening on port %d (reactor + %d workers)",
             config_.listen_port, config_.thread_count);

    // ---- 7. 启动事件循环（阻塞直到 stop() 被调用）
    event_loop_->run(1000);
}