#include "gateway/config.h"
#include "gateway/filter_logging.h"
#include "gateway/filter_request_id.h"
#include "gateway/gateway.h"

int main() {
    GatewayConfig cfg = load_default_config();
    Gateway gw(cfg);

    // 注册过滤器：请求 ID 先执行，日志后执行
    gw.add_filter(std::make_unique<RequestIdFilter>());
    gw.add_filter(std::make_unique<LoggingFilter>());

    gw.run();
    return 0;
}