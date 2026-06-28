#pragma once

#include "gateway/http_request.h"
#include "gateway/http_response.h"

#include <ctime>
#include <string>

// 过滤器上下文，在请求处理链路中传递
struct FilterContext {
    HttpRequest* req = nullptr;       // 原始请求
    HttpResponse* resp = nullptr;     // 响应（仅 on_response 阶段有效）
    std::string request_id;           // 请求唯一标识
    std::time_t start_time = 0;       // 请求到达时间
};

// 过滤器基类
class Filter {
public:
    virtual ~Filter() = default;

    // 请求到达时调用（在路由之前），返回 false 则中断处理链
    virtual bool on_request(FilterContext& /*ctx*/) { return true; }

    // 响应生成后调用（在发送给客户端之前）
    virtual void on_response(FilterContext& /*ctx*/) {}
};