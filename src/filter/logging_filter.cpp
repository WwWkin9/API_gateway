#include "gateway/filter/logging_filter.h"
#include "gateway/logger/logger.h"

#include <ctime>

bool LoggingFilter::on_request(FilterContext& ctx) {
    ctx.start_time = std::time(nullptr);
    const auto& req = *ctx.req;
    LOG_INFO("%s -> %s %s", ctx.request_id.c_str(), req.method.c_str(), req.path.c_str());
    return true;
}

void LoggingFilter::on_response(FilterContext& ctx) {
    std::time_t now = std::time(nullptr);
    double elapsed = difftime(now, ctx.start_time);
    const auto& req = *ctx.req;
    int status = ctx.resp ? ctx.resp->status_code : 0;
    LOG_INFO("%s <- %s %s %d (%.1fs)", ctx.request_id.c_str(),
             req.method.c_str(), req.path.c_str(), status, elapsed);
}