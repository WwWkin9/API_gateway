#include "gateway/http_request.h"

#include <algorithm>
#include <cctype>

// 原地小写化
static void to_lower_inplace(std::string& s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
}

// 解析请求行
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

// 解析请求
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

// keep-alive 判断
bool request_keep_alive(const HttpRequest& req) {
    auto it = req.headers.find("connection");
    if (it != req.headers.end()) {
        std::string v = it->second;
        to_lower_inplace(v);
        if (v.find("keep-alive") != std::string::npos) return true;
    }

    if (req.version == "HTTP/1.1") return true;
    return false;
}