#include "gateway/proxy/proxy.h"
#include "gateway/utils/utils.h"   // set_nonblocking

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

Proxy::Proxy(int backend_timeout_ms) : backend_timeout_ms_(backend_timeout_ms) {}

// ============== 非阻塞 connect ==============

int Proxy::nb_connect(int fd, const sockaddr* addr, socklen_t len, int timeout_ms) {
    set_nonblocking(fd);

    int ret = connect(fd, addr, len);
    if (ret == 0) return 0;
    if (errno != EINPROGRESS) return -1;

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return -1;  // timeout or error

    int err = 0;
    socklen_t errlen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) return -1;
    if (err != 0) return -1;

    return 0;
}

// ============== 非阻塞发送全部数据 ==============

ssize_t Proxy::nb_send_all(int fd, const char* data, size_t len, int timeout_ms) {
    size_t sent = 0;
    while (sent < len) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) return -1;

        ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        } else {
            return -1;
        }
    }
    return static_cast<ssize_t>(sent);
}

// ============== 502 响应 ==============

std::string Proxy::make_502_response() {
    static const std::string resp =
        "HTTP/1.1 502 Bad Gateway\r\n"
        "Content-Length: 11\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Bad Gateway";
    return resp;
}

// ============== 转发请求（调度入口）==============

std::string Proxy::forward(const Backend& backend, const std::string& raw_request) const {
    if (pool_) {
        return forward_pooled(backend, raw_request);
    }
    return forward_direct(backend, raw_request);
}

// ============== 直接转发（短连接）==============

std::string Proxy::forward_direct(const Backend& backend, const std::string& raw_request) const {
    // 1. 创建 socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return make_502_response();

    // 2. 构造地址
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(backend.port);
    if (::inet_pton(AF_INET, backend.host.c_str(), &addr.sin_addr) <= 0) {
        ::close(fd);
        return make_502_response();
    }

    // 3. 连接后端
    if (nb_connect(fd, (sockaddr*)&addr, sizeof(addr), backend_timeout_ms_) < 0) {
        ::close(fd);
        return make_502_response();
    }

    // 4. 发送请求
    if (nb_send_all(fd, raw_request.data(), raw_request.size(), backend_timeout_ms_) < 0) {
        ::close(fd);
        return make_502_response();
    }

    // 5. 确保非阻塞
    set_nonblocking(fd);

    // 6. 读取响应
    std::string resp;
    char buf[4096];
    int tmo = backend_timeout_ms_;
    bool got_data = false;

    while (true) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        // 已收到数据后缩短超时，避免等满整个 timeout_ms
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
    ::close(fd);
    return resp.empty() ? make_502_response() : resp;
}

// ============== 连接池转发（连接复用）==============

std::string Proxy::forward_pooled(const Backend& backend, const std::string& raw_request) const {
    // 1. 从连接池获取连接
    auto conn = pool_->acquire(backend, backend_timeout_ms_);
    if (!conn) {
        return make_502_response();
    }

    // 2. 发送请求
    if (!conn->send_all(raw_request.data(), raw_request.size(), backend_timeout_ms_)) {
        pool_->evict(backend, std::move(conn));
        return make_502_response();
    }

    // 3. 读取响应
    std::string resp = conn->recv_all(backend_timeout_ms_);
    if (resp.empty()) {
        pool_->evict(backend, std::move(conn));
        return make_502_response();
    }

    // 4. 归还连接到池中
    pool_->release(backend, std::move(conn));
    return resp;
}
