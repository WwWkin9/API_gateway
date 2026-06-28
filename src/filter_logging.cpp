#include "gateway/filter_logging.h"

#include <ctime>
#include <iostream>

bool LoggingFilter::on_request(FilterContext& ctx) {
    ctx.start_time = std::time(nullptr);
    const auto& req = *ctx.req;
    std::cout << "[INFO] " << ctx.request_id << " -> " << req.method
              << " " << req.path << std::endl;
    return true;
}

void LoggingFilter::on_response(FilterContext& ctx) {
    std::time_t now = std::time(nullptr);
    double elapsed = difftime(now, ctx.start_time);
    const auto& req = *ctx.req;
    int status = ctx.resp ? ctx.resp->status_code : 0;
    std::cout << "[INFO] " << ctx.request_id << " <- " << req.method
              << " " << req.path << " " << status
              << " (" << elapsed << "s)" << std::endl;
}