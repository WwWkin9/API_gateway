#pragma once

#include "gateway/core/config.h"  // GatewayConfig, Route, Backend

#include <string>
#include <optional>

// 配置加载器：从 JSON 文件加载网关配置
//
// 支持的 JSON 结构：
// {
//   "listen_port": 8080,
//   "keep_alive_timeout_sec": 10,
//   "thread_count": 4,
//   "backend_timeout_ms": 3000,
//   "max_request_size": 65536,
//   "max_epoll_events": 512,
//   "idle_cleanup_interval_sec": 5,
//   "pool_max_idle_per_host": 10,
//   "pool_idle_timeout_sec": 60,
//   "routes": [
//     {
//       "prefix": "/api/user",
//       "backends": [
//         { "host": "127.0.0.1", "port": 9001 },
//         { "host": "127.0.0.1", "port": 9002 }
//       ]
//     }
//   ]
// }
//
// 特点：
//   - 零外部依赖，纯手工递归下降解析器
//   - 宽松解析：跳过空白、支持注释 // 和
//   - 字符串只支持双引号，数字支持整数和浮点（浮点会截断为 int）
//   - 未知键静默忽略，便于向前兼容
//
// 用法：
//   auto cfg = ConfigLoader::load("gateway.json");
//   if (cfg.has_value()) {
//       Gateway gw(cfg.value());
//   } else {
//       // 降级：使用默认配置
//   }

class ConfigLoader {
public:
    // 从文件加载配置，解析失败返回 std::nullopt
    static std::optional<GatewayConfig> load(const std::string& filepath);

    // 从 JSON 字符串加载配置
    static std::optional<GatewayConfig> load_json(const std::string& json);
};

// 错误信息（用于调试）
const char* config_loader_last_error();
