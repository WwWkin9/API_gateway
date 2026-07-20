#include "gateway/core/config.h"
#include "gateway/core/config_loader.h"
#include "gateway/core/gateway.h"
#include "gateway/filter/logging_filter.h"
#include "gateway/filter/request_id_filter.h"
#include "gateway/filter/rate_limit_filter.h"
#include "gateway/logger/logger.h"

int main(int argc, char* argv[]) {
    // 初始化日志
    Logger::instance().set_level(LogLevel::INFO);

    // 尝试从配置文件加载，失败降级为默认配置
    GatewayConfig cfg;
    const char* config_path = "config.json";
    bool loaded = false;

    // 检查命令行 --config=
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--config=", 0) == 0) {
            config_path = arg.c_str() + 9;  // strlen("--config=")
        }
    }

    if (auto loaded_cfg = ConfigLoader::load(config_path)) {
        cfg = std::move(loaded_cfg.value());
        loaded = true;
        LOG_INFO("loaded config from %s", config_path);
    }

    if (!loaded) {
        LOG_WARN("failed to load %s, using defaults", config_path);
        cfg = load_default_config();
    }

    // 命令行覆盖端口和线程数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--port=", 0) == 0) {
            cfg.listen_port = std::stoi(arg.substr(7));
        } else if (arg.rfind("--threads=", 0) == 0) {
            cfg.thread_count = std::stoi(arg.substr(10));
        } else if (arg.rfind("--timeout=", 0) == 0) {
            cfg.backend_timeout_ms = std::stoi(arg.substr(10));
        }
    }

    Gateway gw(cfg);

    // 注册过滤器：RequestId → RateLimit → Logging
    gw.add_filter(std::make_unique<RequestIdFilter>());
    gw.add_filter(std::make_unique<RateLimitFilter>(100, 200));
    gw.add_filter(std::make_unique<LoggingFilter>());

    gw.run();
    return 0;
}