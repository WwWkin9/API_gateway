#include "gateway/utils/utils.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 128) < 0) {
        close(fd);
        return -1;
    }

    if (set_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

// 安全解析十进制整数（C++17 from_chars 的轻量替代，避免 stoi 抛异常）
std::optional<int> parse_int_safe(const char* begin, const char* end) {
    if (begin >= end) return std::nullopt;
    char* last = nullptr;
    long val = std::strtol(begin, &last, 10);
    if (last != end) return std::nullopt;   // 有非数字后缀
    if (val < 0 || val > 2147483647) return std::nullopt;  // 拒绝负数和溢出
    return static_cast<int>(val);
}

// 读取完整请求，支持 body；max_size 为 0 或负数表示不设上限
std::string recv_all_request(int fd, int max_size) {
    std::string data;
    data.reserve(8192);
    char buf[4096];

    int content_length = -1;
    bool headers_complete = false;
    const int effective_max = (max_size > 0) ? max_size : 2147483647;

    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            data.append(buf, n);

            // 防 DoS：在读全 headers 之前也限制缓冲区膨胀
            if ((int)data.size() > effective_max) {
                data.clear();
                break;
            }

            if (!headers_complete) {
                size_t header_end = data.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    headers_complete = true;

                    size_t cl_pos = data.find("Content-Length:", 0, header_end);
                    if (cl_pos != std::string::npos) {
                        cl_pos += 15;
                        while (cl_pos < header_end &&
                               (data[cl_pos] == ' ' || data[cl_pos] == '\t')) {
                            ++cl_pos;
                        }

                        size_t cl_end = data.find("\r\n", cl_pos);
                        if (cl_end == std::string::npos || cl_end > header_end) {
                            cl_end = header_end;
                        }

                        // 安全解析，避免 stoi 对非数字抛异常
                        auto parsed = parse_int_safe(
                            data.c_str() + cl_pos,
                            data.c_str() + cl_end);
                        if (parsed.has_value() && parsed.value() >= 0) {
                            content_length = parsed.value();
                        }
                        // 解析失败时 content_length 保持 -1，不等待 body

                        // 拒绝超大 Content-Length
                        if (content_length > effective_max) {
                            data.clear();
                            break;
                        }
                    }
                }
            }

            if (headers_complete) {
                if (content_length >= 0) {
                    size_t body_start = data.find("\r\n\r\n") + 4;
                    if (data.size() - body_start >= (size_t)content_length) {
                        data.resize(body_start + content_length);
                        break;
                    }
                } else {
                    break;
                }
            }

            if (n < (ssize_t)sizeof(buf) && !headers_complete) break;
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }
    }

    return data;
}