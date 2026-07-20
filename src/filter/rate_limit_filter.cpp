#include "gateway/filter/rate_limit_filter.h"

#include <algorithm>

RateLimitFilter::RateLimitFilter(int rate_per_second, int burst)
    : rate_(rate_per_second)
    , burst_(burst)
    , token_per_ns_(static_cast<double>(rate_per_second) / 1'000'000'000.0)
    , key_extractor_(extract_client_ip)
{}

void RateLimitFilter::set_key_extractor(KeyExtractor extractor) {
    key_extractor_ = std::move(extractor);
}

bool RateLimitFilter::on_request(FilterContext& ctx) {
    if (rate_ <= 0) return true; // 无限流
    
    // 从请求头中提取客户端 IP（默认 X-Forwarded-For 或 socket）
    std::string key = key_extractor_ ? key_extractor_(ctx) : "";
    if (key.empty()) return true; // 不限流

    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();

    auto it = token_buckets_.find(key);
    if (it == token_buckets_.end()) {
        it = token_buckets_.emplace(key, TokenBucket{static_cast<double>(burst_), 
            now}).first;
    }

    auto& bucket = it->second;

    // 填充桶（根据当前时间）
    double elapsed_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - bucket.last_fill_time).count());

    bucket.tokens += elapsed_ns * token_per_ns_;
    if (bucket.tokens > burst_) {
        bucket.tokens = burst_;
    }
    bucket.last_fill_time = now;

    // 消费令牌
    if (bucket.tokens >= 1.0) {
        bucket.tokens -= 1.0;
        return true;
    }
    return false;
}   

void RateLimitFilter::cleanup_expired_buckets(int idle_sec) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    auto threshold = std::chrono::seconds(idle_sec);

    auto it = token_buckets_.begin();
    while (it != token_buckets_.end()) {
        if (now - it->second.last_fill_time > threshold) {
            it = token_buckets_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t RateLimitFilter::active_buckets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return token_buckets_.size();
}

// ============== 默认 IP 提取 ==============

std::string RateLimitFilter::extract_client_ip(const FilterContext& ctx) {
    if (!ctx.req) return "";

    // 优先取 X-Forwarded-For（第一个 IP）
    auto it = ctx.req->headers.find("x-forwarded-for");
    if (it != ctx.req->headers.end()) {
        const std::string& val = it->second;
        size_t comma = val.find(',');
        std::string ip = (comma != std::string::npos)
                         ? val.substr(0, comma) : val;

        // 去除前后空格
        size_t start = ip.find_first_not_of(" \t");
        size_t end   = ip.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos) {
            return ip.substr(start, end - start + 1);
        }
        return ip;
    }

    // 降级：用 X-Real-IP
    it = ctx.req->headers.find("x-real-ip");
    if (it != ctx.req->headers.end()) {
        return it->second;
    }

    return "";  // 无 IP 信息，不限流
}