#include "gateway/proxy/health_checker.h"
#include "gateway/utils/utils.h"  // set_nonblocking

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <sstream>

// ============== 工具：生成 key ==============

std::string HealthChecker::make_key(const std::string& host, int port) {
    std::ostringstream oss;
    oss << host << ':' << port;
    return oss.str();
}

std::string HealthChecker::make_key(const Backend& backend) {
    return make_key(backend.host, backend.port);
}

// ============== 后端管理 ==============

void HealthChecker::add_backend(const Backend& backend) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = make_key(backend);
    if (backends_.find(key) == backends_.end()) {
        backends_[key] = BackendHealth{backend, false, {}, 0};
    }
}

void HealthChecker::remove_backend(const Backend& backend) {
    std::lock_guard<std::mutex> lock(mutex_);
    backends_.erase(make_key(backend));
}

void HealthChecker::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    backends_.clear();
}

// ============== TCP 连接检查 ==============

bool HealthChecker::tcp_check(const Backend& backend) const {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    set_nonblocking(fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(backend.port);
    if (::inet_pton(AF_INET, backend.host.c_str(), &addr.sin_addr) <= 0) {
        ::close(fd);
        return false;
    }

    int ret = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (ret == 0) {
        ::close(fd);
        return true;  // 立即连接成功
    }

    if (errno != EINPROGRESS) {
        ::close(fd);
        return false;
    }

    // 等待连接完成
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    ret = ::poll(&pfd, 1, connect_timeout_ms_);
    if (ret <= 0) {
        ::close(fd);
        return false;
    }

    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
        ::close(fd);
        return false;
    }

    ::close(fd);
    return true;
}

// ============== 执行健康检查 ==============

void HealthChecker::check_all() {
    // 收集所有后端（锁内快照）
    std::vector<BackendHealth> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot.reserve(backends_.size());
        for (auto& [key, bh] : backends_) {
            snapshot.push_back(bh);
        }
    }

    auto now = std::chrono::steady_clock::now();

    for (auto& bh : snapshot) {
        bool was_healthy = bh.healthy;
        bool now_healthy = tcp_check(bh.backend);

        bh.last_check = now;

        if (now_healthy) {
            bh.consecutive_failures = 0;
            bh.healthy = true;
        } else {
            bh.consecutive_failures++;
            // 连续失败 3 次才标记为不健康（避免网络抖动）
            if (bh.consecutive_failures >= 3) {
                bh.healthy = false;
            }
        }

        // 状态变更时触发回调（用更新后的 bh.healthy，考虑连续失败阈值）
        if (bh.healthy != was_healthy) {
            if (on_status_change_) {
                on_status_change_(bh.backend, bh.healthy);
            }
        }

        // 写回
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::string key = make_key(bh.backend);
            auto it = backends_.find(key);
            if (it != backends_.end()) {
                it->second = bh;
            }
        }
    }
}

// ============== 查询 ==============

bool HealthChecker::is_healthy(const Backend& backend) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = backends_.find(make_key(backend));
    return it != backends_.end() && it->second.healthy;
}

std::vector<Backend> HealthChecker::healthy_backends() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Backend> result;
    result.reserve(backends_.size());
    for (const auto& [key, bh] : backends_) {
        if (bh.healthy) {
            result.push_back(bh.backend);
        }
    }
    return result;
}

size_t HealthChecker::backend_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backends_.size();
}

std::vector<HealthChecker::BackendHealth> HealthChecker::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BackendHealth> result;
    result.reserve(backends_.size());
    for (const auto& [key, bh] : backends_) {
        result.push_back(bh);
    }
    return result;
}
