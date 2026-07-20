#pragma once

#include <string>
#include <map>

// HTTP 请求结构
struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;
};

// 判断是否 keep-alive
bool request_keep_alive(const HttpRequest& req);