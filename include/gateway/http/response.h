#pragma once

#include <string>
#include <map>

// HTTP 响应结构
struct HttpResponse {
    int status_code = 200;
    std::string status_text = "OK";
    std::map<std::string, std::string> headers;
    std::string body;

    // 序列化成 HTTP 报文
    std::string to_string() const;
};

// 常见错误响应
HttpResponse make_400(bool keep_alive);
HttpResponse make_404(bool keep_alive);
HttpResponse make_502(bool keep_alive);