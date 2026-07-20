#pragma once

#include <string>
#include <optional>

// 安全解析十进制整数，失败返回 nullopt
std::optional<int> parse_int_safe(const char* begin, const char* end);

// 读取完整请求（max_size 为 0 或负数时表示不限制）
std::string recv_all_request(int fd, int max_size);