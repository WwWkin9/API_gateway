#pragma once

#include "gateway/filter/filter.h"

// 请求 ID 过滤器：为每个请求生成或透传 X-Request-Id
class RequestIdFilter : public Filter {
public:
    bool on_request(FilterContext& ctx) override;
    void on_response(FilterContext& ctx) override;
};