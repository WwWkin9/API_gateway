#include "gateway/core/config.h"
#include "gateway/core/config_loader.h"
#include "gateway/core/gateway.h"
#include "gateway/filter/logging_filter.h"
#include "gateway/filter/request_id_filter.h"
#include "gateway/filter/rate_limit_filter.h"
#include "gateway/logger/async_logger.h"
#include "gateway/logger/logger.h"

#include <csignal>
#include <cstring>
#include <memory>
#include <sys/stat.h>

// 命令行日志级别解析
static LogLevel parse_log_level(const char* s) {
    if (strcasecmp(s, "trace") == 0) return LogLevel::TRACE;
    if (strcasecmp(s, "debug") == 0) return LogLevel::DEBUG;
    if (strcasecmp(s, "info")  == 0) return LogLevel::INFO;
    if (strcasecmp(s, "warn")  == 0 || strcasecmp(s, "warning") == 0) return LogLevel::WARN;
    if (strcasecmp(s, "error") == 0) return LogLevel::ERROR;
    if (strcasecmp(s, "fatal") == 0) return LogLevel::FATAL;
    return LogLevel::INFO;  // 默认
}

int main(int argc, char* argv[]) {
    // 忽略 SIGPIPE，防止向已关闭的 socket 写入时进程崩溃
    signal(SIGPIPE, SIG_IGN);

    // 初始化日志（默认 INFO，可通过 --log-level=warn 降低开销）
    LogLevel log_level = LogLevel::INFO;

    // 尝试从配置文件加载，失败降级为默认配置
    GatewayConfig cfg;
    const char* config_path = "config.json";
    bool loaded = false;

    // 检查命令行 --config= 和 --log-level=
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--config=", 0) == 0) {
            config_path = arg.c_str() + 9;  // strlen("--config=")
        } else if (arg.rfind("--log-level=", 0) == 0) {
            log_level = parse_log_level(arg.c_str() + 12);  // strlen("--log-level=")
        }
    }

    Logger::instance().set_level(log_level);

    mkdir("/home/lin/API_gateway/logs", 0755);

    Logger::instance().set_async_logger(
        std::make_unique<AsyncLogger>("/home/lin/API_gateway/logs/gateway.log", 1000, 1024 * 1024 * 100));

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