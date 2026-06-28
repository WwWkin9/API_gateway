#pragma once

#include "gateway/filter.h"

// 日志过滤器：记录请求方法、路径、状态码和耗时
class LoggingFilter : public Filter {
public:
    bool on_request(FilterContext& ctx) override;
    void on_response(FilterContext& ctx) override;
};