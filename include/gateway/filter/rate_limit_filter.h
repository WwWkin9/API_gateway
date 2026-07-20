#pragma once

#include "gateway/filter/filter.h"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <functional>

// 限流过滤器：基于令牌桶算法限制请求频率
//
// 用法：
//   RateLimitFilter limiter(100, 200);  // 每秒100个请求，突发200
//   gw.add_filter(std::make_unique<RateLimitFilter>(100, 200));
//
// 限流键：默认按客户端 IP。可通过 set_key_extractor 自定义

class RateLimitFilter : public Filter {
public:
    // rate:  每秒生成的令牌数（稳态 QPS）
    // burst: 桶容量（允许的最大突发请求数）
    explicit RateLimitFilter(int rate_per_second = 100, int burst = 200);

    // on_request 返回 false 表示超过限流阈值，中断处理链
    bool on_request(FilterContext& ctx) override;

    // 设置限流键提取函数（默认从 X-Forwarded-For 或 socket 获取 IP）
    // 返回空字符串表示不限流
    using KeyExtractor = std::function<std::string(const FilterContext&)>;
    void set_key_extractor(KeyExtractor extractor);

    // 清理过期的桶（空闲回调中周期性调用）
    void cleanup_expired_buckets(int idle_sec = 300);

    // 当前活跃桶数量（用于监控）
    size_t active_buckets() const;

private:
    struct TokenBucket {
       double tokens;
       std::chrono::steady_clock::time_point last_fill_time;
    };

    int rate_;
    int burst_;
    double token_per_ns_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, TokenBucket> token_buckets_;
    KeyExtractor key_extractor_;

    // 填充桶（根据当前时间）
    // 每个请求会从桶中消耗一个令牌
    void refill(TokenBucket& bucket);

    // 从请求头中提取客户端 IP（默认 X-Forwarded-For 或 socket）
    // 返回空字符串表示不限流
    static std::string extract_client_ip(const FilterContext& ctx);
   };
