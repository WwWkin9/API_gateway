#include "gateway/core/gateway.h"

#include "gateway/http/parser.h"   // parse_http_request, HttpRequest
#include "gateway/http/request.h"  // request_keep_alive
#include "gateway/http/response.h"
#include "gateway/logger/logger.h"
#include "gateway/monitor/stats.h"
#include "gateway/proxy/backend_pool.h"
#include "gateway/utils/utils.h"
#include "gateway/filter/rate_limit_filter.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <ctime>
#include <cstring>
#include <sstream>

Gateway::Gateway(const GatewayConfig& config) : config_(config) {
    LOG_INFO("Gateway: config has %zu routes", config.routes.size());
    max_connections_ = config.max_connections;
    router_ = std::make_unique<Router>(config.routes);
    proxy_ = std::make_unique<Proxy>(config.backend_timeout_ms);
    load_balancer_ = std::make_unique<LoadBalancer>(LBAlgorithm::RoundRobin);

    // 创建后端连接池并注入 Proxy
    auto pool = std::make_shared<BackendPool>(config.pool_max_idle_per_host,
                                              config.pool_idle_timeout_sec);
    proxy_->set_pool(std::move(pool));
}

void Gateway::add_filter(std::unique_ptr<Filter> filter) {
    // 捕获 RateLimitFilter 指针，用于定时清理
    if (auto* rlf = dynamic_cast<RateLimitFilter*>(filter.get())) {
        rate_limit_filter_ = rlf;
    }
    filters_.push_back(std::move(filter));
}

void Gateway::process_request(std::shared_ptr<Connection> conn, std::string raw) {
    int fd = conn->fd();

    // 1. 解析请求
    HttpRequest req;
    if (!parse_http_request(raw, req)) {
        LOG_WARN("process_request fd=%d parse failed", fd);
        send_error_and_close(conn, make_400(false).to_string());
        return;
    }

    LOG_DEBUG("process_request fd=%d path=[%s] method=[%s]", fd, req.path.c_str(), req.method.c_str());

    // 2. 检查内部端点
    std::string internal_resp;
    if (handle_internal_endpoint(req.path, internal_resp)) {
        LOG_DEBUG("process_request fd=%d internal endpoint %s", fd, req.path.c_str());
        LOG_DEBUG("process_request fd=%d sending internal resp len=%zu", fd, internal_resp.size());
        conn->send(internal_resp, false);
        return;
    }

    // 3. 过滤器链：请求阶段
    FilterContext fctx;
    fctx.req = &req;
    for (auto& f : filters_) {
        if (!f->on_request(fctx)) {
            GatewayStats::instance().inc_rate_limited();
            send_error_and_close(conn, make_400(false).to_string());
            return;
        }
    }

    auto start_time = std::chrono::steady_clock::now();

    // 4. 路由 + 负载均衡
    auto backends = router_->match(req.path);
    if (!backends.has_value()) {
        LOG_WARN("process_request fd=%d route NOT FOUND for path=[%s]", fd, req.path.c_str());
        send_error_and_close(conn, make_404(false).to_string());
        return;
    }
    LOG_DEBUG("process_request fd=%d route matched, backends=%zu", fd, backends->size());
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
        send_error_and_close(conn, resp503);
        return;
    }

    // 5. 转发到后端
    load_balancer_->on_connect(target);
    std::string raw_resp = proxy_->forward(target, raw);
    load_balancer_->on_disconnect(target);

    // 6. 统计 + 断路器上报
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();

    bool success = !raw_resp.empty() && raw_resp.find("HTTP/1.1 5") != 0;
    GatewayStats::instance().record_request(req.path, elapsed);
    if (success) {
        GatewayStats::instance().record_success(req.path);
        cb.on_success();
    } else {
        GatewayStats::instance().record_error(req.path);
        GatewayStats::instance().inc_upstream_errors();
        cb.on_failure();
    }

    std::string out = raw_resp;

    // 找到响应头尾分隔点，后续多次复用，避免重复扫描
    size_t hdr_end = out.find("\r\n\r\n");

    if (hdr_end != std::string::npos) {
        std::string_view head(out.data(), hdr_end + 2);
        bool has_conn = (::memmem(head.data(), head.size(), "\r\nConnection:", 13) != nullptr);
        if (!has_conn) {
            has_conn = (::memmem(head.data(), head.size(), "\r\nconnection:", 13) != nullptr);
        }
        if (!has_conn) {
            out.insert(hdr_end, "\r\nConnection: close");
            hdr_end += 21;
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

        // 在已缓存的 hdr_end 前插入过滤器新增的响应头
        if (!tmp_resp.headers.empty() && hdr_end != std::string::npos) {
            std::string extra;
            extra.reserve(tmp_resp.headers.size() * 32);
            for (const auto& [k, v] : tmp_resp.headers) {
                // 只在响应头区域中没有该头时才追加
                std::string tag = "\r\n" + k + ":";
                const void* hit = ::memmem(out.data(), hdr_end, tag.data(), tag.size());
                if (!hit) {
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

    // 9. 通过 Connection 发送（直接使用持有的 shared_ptr，避免 fd 复用问题）
    bool ka = request_keep_alive(req);
    // 如果后端响应要求关闭连接，则不应保持客户端连接
    if (ka && (out.find("\r\nConnection: close") != std::string::npos ||
               out.find("\r\nconnection: close") != std::string::npos)) {
        ka = false;
    }
    conn->send(out, ka);
}

CircuitBreaker& Gateway::get_circuit_breaker(const Backend& backend) {
    std::string key;
    key.reserve(backend.host.size() + 12);
    key += backend.host;
    key += ':';
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", backend.port);
    key += port_buf;

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
        std::move(key), 5, config_.backend_timeout_ms * 2, 3
    ).first->second;
}

void Gateway::send_error_and_close(std::shared_ptr<Connection> conn, const std::string& resp) {
    conn->send(resp, false);
}

void Gateway::cleanup_idle_connections() {
    std::time_t now = std::time(nullptr);
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.begin();
    while (it != connections_.end()) {
        if (now - it->second->last_active_time() > config_.keep_alive_timeout_sec) {
            it->second->force_close();         // 延迟关闭，close_callback 会清理计数
            it = connections_.erase(it);       // 从 map 移除（close_callback 会做双重擦除，无害）
        } else {
            ++it;
        }
    }
}

std::vector<Backend> Gateway::collect_all_backends() const {
    std::vector<Backend> result;
    for (const auto& route : config_.routes) {
        for (const auto& b : route.backends) {
            result.push_back(b);
        }
    }
    // 去重
    std::sort(result.begin(), result.end(),
        [](const Backend& a, const Backend& b) {
            return a.host < b.host || (a.host == b.host && a.port < b.port);
        });
    result.erase(
        std::unique(result.begin(), result.end(),
            [](const Backend& a, const Backend& b) {
                return a.host == b.host && a.port == b.port;
            }),
        result.end());
    return result;
}

bool Gateway::handle_internal_endpoint(const std::string& path, std::string& response) {
    // /stats - JSON 格式统计
    if (path == "/stats" || path == "/stats/") {
        auto snap = GatewayStats::instance().global_snapshot();
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: application/json\r\n"
            << "Connection: close\r\n"
            << "\r\n"
            << "{"
            << "\"total_requests\":" << snap.total_requests << ","
            << "\"total_success\":" << snap.total_success << ","
            << "\"total_errors\":" << snap.total_errors << ","
            << "\"avg_latency_ms\":" << snap.avg_latency_ms << ","
            << "\"max_latency_ms\":" << snap.max_latency_ms << ","
            << "\"in_flight\":" << snap.in_flight << ","
            << "\"upstream_errors\":" << snap.upstream_errors << ","
            << "\"circuit_breaker_trips\":" << snap.circuit_breaker_trips << ","
            << "\"rate_limited\":" << snap.rate_limited;
        // 添加各路由统计
        auto route_stats = GatewayStats::instance().route_snapshot();
        if (!route_stats.empty()) {
            oss << ",\"routes\":{";
            bool first = true;
            for (const auto& [name, rs] : route_stats) {
                if (!first) oss << ",";
                first = false;
                oss << "\"" << name << "\":{"
                    << "\"requests\":" << rs.total_requests << ","
                    << "\"success\":" << rs.success_count << ","
                    << "\"errors\":" << rs.error_count << ","
                    << "\"avg_ms\":" << rs.avg_latency_ms
                    << "}";
            }
            oss << "}";
        }
        oss << "}";
        response = oss.str();
        return true;
    }

    // /health - 健康检查
    if (path == "/health" || path == "/health/") {
        response = "HTTP/1.1 200 OK\r\n"
                   "Content-Length: 2\r\n"
                   "Connection: close\r\n"
                   "\r\n"
                   "OK";
        return true;
    }

    return false;
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
    event_loop_->set_max_deferred_per_round(config_.max_deferred_per_round);

    // ---- 3. 创建线程池 ----
    pool_ = std::make_unique<ThreadPool>(config_.thread_count, config_.max_queue_size);

    // ---- 4. 注册 listen_fd 的回调: accept 接受新连接 ----
    tcp_server_ = std::make_unique<TCPServer>(event_loop_.get(), config_.listen_port);
    tcp_server_->set_new_connection_callback([this](int cfd, std::string peer_ip, int peer_port){
        (void)peer_ip;
        (void)peer_port;

        // 连接数上限检查：超限时直接关闭新连接，避免 OOM
        if (max_connections_ > 0 && connection_count_.load() >= max_connections_) {
            ::close(cfd);
            LOG_WARN("Gateway: connection limit reached (%d), rejecting new connection",
                     max_connections_);
            return;
        }

        auto conn = std::make_shared<Connection>(
               event_loop_.get(), cfd, config_.max_request_size
        );
      
        conn->set_message_callback([this, conn](int /*fd*/, std::string raw) {
            pool_->submit([this, conn, raw = std::move(raw)]() {
                process_request(conn, std::move(raw));
            });
        });
        conn->set_close_callback([this](int fd) {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_.erase(fd);
            connection_count_--;
        });
        event_loop_->add_fd(cfd, EPOLLIN | EPOLLET);
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
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_[cfd] = conn;
        }
        connection_count_++;
    });
    tcp_server_->start();

    // ---- 5. 注册周期性定时任务 ----
    // 空闲连接清理
    int cleanup_ms = config_.idle_cleanup_interval_sec * 1000;
    timer_->run_every(cleanup_ms, [this]() {
        cleanup_idle_connections();
        proxy_->cleanup_pool();

        // 清理限流器中 5 分钟未活跃的桶
        if (rate_limit_filter_) {
            rate_limit_filter_->cleanup_expired_buckets(300);
        }
    });

    // ---- 6. 初始化健康检查器 ----
    health_checker_ = std::make_unique<HealthChecker>();
    health_checker_->set_interval_ms(5000);
    health_checker_->set_connect_timeout_ms(config_.backend_timeout_ms);
    health_checker_->set_on_status_change(
        [](const Backend& backend, bool healthy) {
            if (healthy) {
                LOG_INFO("backend %s:%d is healthy", backend.host.c_str(), backend.port);
            } else {
                LOG_WARN("backend %s:%d is UNHEALTHY", backend.host.c_str(), backend.port);
            }
        });

    auto all_backends = collect_all_backends();
    for (const auto& b : all_backends) {
        health_checker_->add_backend(b);
    }

    // 每 5 秒执行一次健康检查
    timer_->run_every(5000, [this]() {
        health_checker_->check_all();
    });

    // ---- 7. 注册空闲回调：驱动定时器 tick ----
    event_loop_->set_idle_callback([this]() {
        timer_->tick();
    });

    LOG_INFO("Gateway listening on port %d (reactor + %d workers)",
             config_.listen_port, config_.thread_count);

    // ---- 7. 启动事件循环（阻塞直到 stop() 被调用）
    event_loop_->run(1000);
}