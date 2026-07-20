#include "gateway/core/config.h"
#include "gateway/core/gateway.h"
#include "gateway/filter/logging_filter.h"
#include "gateway/filter/request_id_filter.h"

int main() {
    GatewayConfig cfg = load_default_config();
    Gateway gw(cfg);

    // 注册过滤器：请求 ID 先执行，限流后执行，日志后执行
    gw.add_filter(std::make_unique<RequestIdFilter>());
    gw.add_filter(std::make_unique<RateLimitFilter>(100, 200));
    gw.add_filter(std::make_unique<LoggingFilter>());


    gw.run();
    return 0;
}