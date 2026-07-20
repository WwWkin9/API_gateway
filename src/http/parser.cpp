#include "gateway/http/parser.h"
#include "gateway/utils/utils.h"  // parse_int_safe

#include <algorithm>
#include <cctype>
#include <cstring>

// ============== 工具 ==============

static void to_lower_inplace(std::string& s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
}

// ============== Content-Length 解析 ==============

int parse_content_length(const char* header_data, size_t header_len) {
    const char* cl_pos = static_cast<const char*>(
        memmem(header_data, header_len, "Content-Length:", 15));
    if (!cl_pos) return -1;

    cl_pos += 15;
    // 跳过空格/制表符
    while (cl_pos < header_data + header_len &&
           (*cl_pos == ' ' || *cl_pos == '\t')) {
        ++cl_pos;
    }

    const char* header_end = header_data + header_len;
    const char* cl_end = static_cast<const char*>(
        memmem(cl_pos, header_end - cl_pos, "\r\n", 2));
    if (!cl_end) cl_end = header_end;

    auto parsed = parse_int_safe(cl_pos, cl_end);
    if (parsed.has_value() && parsed.value() >= 0) {
        return parsed.value();
    }
    return -1;
}

// ============== 请求行解析 ==============

static bool parse_request_line(const std::string& line, HttpRequest& req) {
    size_t pos1 = line.find(' ');
    if (pos1 == std::string::npos) return false;

    size_t pos2 = line.find(' ', pos1 + 1);
    if (pos2 == std::string::npos) return false;

    req.method = line.substr(0, pos1);
    req.path = line.substr(pos1 + 1, pos2 - pos1 - 1);
    req.version = line.substr(pos2 + 1);

    return !req.method.empty() && !req.path.empty() && !req.version.empty();
}

// ============== HTTP 请求解析 ==============

bool parse_http_request(const std::string& raw, HttpRequest& req) {
    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;

    req.body = raw.substr(header_end + 4);

    size_t pos = 0;
    size_t line_start = 0;

    while (pos <= header_end) {
        if (raw[pos] == '\r' && pos + 1 <= header_end && raw[pos + 1] == '\n') {
            size_t line_len = pos - line_start;
            if (line_len > 0) {
                std::string line = raw.substr(line_start, line_len);

                if (req.method.empty()) {
                    if (!parse_request_line(line, req)) return false;
                } else {
                    size_t colon_pos = line.find(':');
                    if (colon_pos != std::string::npos) {
                        std::string key = line.substr(0, colon_pos);
                        to_lower_inplace(key);

                        size_t value_start = colon_pos + 1;
                        while (value_start < line.size() &&
                               (line[value_start] == ' ' || line[value_start] == '\t')) {
                            ++value_start;
                        }
                        std::string value = line.substr(value_start);
                        req.headers[key] = value;
                    }
                }
            }
            pos += 2;
            line_start = pos;
        } else {
            ++pos;
        }
    }

    return !req.method.empty() && !req.path.empty() && !req.version.empty();
}
