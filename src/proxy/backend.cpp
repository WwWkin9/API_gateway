#include "gateway/proxy/backend.h"
#include "gateway/net/socket.h"

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <string_view>

BackendConnection::BackendConnection(const std::string& host, int port)
    : host_(host), port_(port) 
{}

BackendConnection::~BackendConnection() {
    close();
}
        
bool BackendConnection::connect(int timeout_ms) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        return false;
    }
    set_nonblocking(fd_);

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        close();
        return false;
    }

    int ret = ::connect(fd_, (sockaddr*)&addr, sizeof(addr));
    if (ret == 0) {
        touch();
        return true;
    }
    // 非阻塞 connect 失败，检查是否为 EINPROGRESS
    if (errno != EINPROGRESS) {
        close();
        return false;
    }
    
    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLOUT;
    ret = ::poll(&pfd, 1, timeout_ms);
    if (ret <= 0) {
        close();
        return false;
    }
    
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
        close();
        return false;
    }
    touch();
    return true;
}

bool BackendConnection::send_all(const char* data, size_t len, int timeout_ms) {
    if (fd_ < 0) {
        return false;
    }
    size_t sent = 0;
    while (sent < len) {
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLOUT;
        int ret = ::poll(&pfd, 1, timeout_ms);
        if (ret <= 0) return false; 

        ssize_t n = ::send(fd_, data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

std::string BackendConnection::recv_all(int timeout_ms) {
    if (fd_ < 0) return {};

    std::string resp;
    char buf[4096];
    bool got_data = false;
    bool eof = false;
    ssize_t content_length = -1;  // -1 表示尚未解析
    size_t header_end = std::string::npos;

    while (true){
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        // 首次读取使用完整超时，后续读取使用更短超时
        int tmo = got_data ? (timeout_ms > 200 ? 200 : timeout_ms / 2) : timeout_ms;
        int ret = ::poll(&pfd, 1, tmo);
        if (ret < 0) break;
        if (ret == 0) break;

        ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            resp.append(buf, static_cast<size_t>(n));
            got_data = true;

            // 解析 Content-Length（仅解析一次）
            if (content_length < 0) {
                header_end = resp.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    // 查找 Content-Length 头
                    std::string_view headers(resp.data(), header_end);
                    size_t cl_pos = headers.find("\r\nContent-Length:");
                    if (cl_pos == std::string::npos) {
                        cl_pos = headers.find("\r\ncontent-length:");
                    }
                    if (cl_pos != std::string::npos) {
                        // 跳过 "\r\nContent-Length:" 和可能的空格
                        const char* val_start = headers.data() + cl_pos + 17; // len("\r\nContent-Length:") = 17
                        while (val_start < headers.data() + headers.size() && *val_start == ' ') val_start++;
                        content_length = 0;
                        while (val_start < headers.data() + headers.size() && *val_start >= '0' && *val_start <= '9') {
                            content_length = content_length * 10 + (*val_start - '0');
                            val_start++;
                        }
                    } else {
                        // 没有 Content-Length，可能是 chunked 或无 body，设 0 以依赖超时
                        content_length = 0;
                    }
                }
            }

            // 检查是否已收到完整响应
            if (content_length >= 0 && header_end != std::string::npos) {
                size_t total_expected = header_end + 4 + static_cast<size_t>(content_length);
                if (resp.size() >= total_expected) {
                    break;  // 完整响应已收到
                }
            }
        } else if (n == 0) {
            eof = true;  // 对端正常关闭
            break;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        } else {
            break;
        }
    }
    if (!got_data) {
        close();
        return {};
    }
    // 后端连接已关闭（Connection: close），标记 fd 为无效防止归还到池中
    if (eof) {
        close();
    } else {
        touch();
    }
    return resp;
}

bool BackendConnection::is_alive() const {
    if (fd_ < 0) return false;

    // SO_ERROR 检查：连接是否异常
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return false;

    if (err != 0) return false;

    // 额外检查：用非阻塞 peek 看对端是否已关闭
    char c;
    ssize_t n = ::recv(fd_, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
    if (n == 0) return false;

    return true;
   }

void BackendConnection::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}
