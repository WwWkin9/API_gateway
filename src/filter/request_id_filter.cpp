#include "gateway/filter/request_id_filter.h"

#include <chrono>
#include <random>
#include <sstream>

// 生成一个简短的唯一 ID
static std::string generate_id() {
    static thread_local std::mt19937 rng(
        std::chrono::steady_clock::now().time_since_epoch().count());
    static thread_local std::uniform_int_distribution<int> dist(0, 15);

    std::ostringstream oss;
    oss << std::hex;
    for (int i = 0; i < 8; ++i) oss << dist(rng);
    return oss.str();
}

// 请求 ID 过滤器：为每个请求生成或透传 X-Request-Id
bool RequestIdFilter::on_request(FilterContext& ctx) {
    auto it = ctx.req->headers.find("x-request-id");
    if (it != ctx.req->headers.end()) {
        ctx.request_id = it->second; // 透传已有的 request-id
    } else {
        ctx.request_id = generate_id(); // 新生成
    }
    return true;
}

void RequestIdFilter::on_response(FilterContext& ctx) {
    if (ctx.resp) {
        ctx.resp->headers["X-Request-Id"] = ctx.request_id;
    }
}