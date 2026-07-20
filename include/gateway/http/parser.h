#pragma once

#include "gateway/http/request.h"  // HttpRequest

#include <cstddef>
#include <cstdint>
#include <string>

// ============================================================
// 静态工具函数：一次性解析
// ============================================================

// 从原始 HTTP 头部数据中提取 Content-Length 值
// 返回 -1 表示不存在或解析失败
int parse_content_length(const char* header_data, size_t header_len);

// 解析 HTTP 请求报文（一次性、非增量）
bool parse_http_request(const std::string& raw, HttpRequest& req);

// ============================================================
// HttpParser：增量 HTTP 请求状态机
//
// 状态流转：
//   RequestLine ──(收到\r\n)──> Headers
//   Headers ──(收到\r\n\r\n)──> Body / ChunkedBody / Complete
//   Body ──(Content-Length 读完)──> Complete
//   ChunkedBody ──(0\r\n\r\n)──> Complete
//
// 用法：
//   HttpParser parser;
//   while (buf.has_data()) {
//       size_t n = parser.parse(buf.data(), buf.size());
//       buf.consume(n);
//       if (parser.complete()) {
//           std::string raw = parser.raw();  // 完整报文
//           HTTPRequest& req = parser.request();
//           parser.reset();
//       }
//       if (parser.has_error()) { ... }
//       if (n == 0) break;  // 数据不足，等待下次
//   }
// ============================================================

class HttpParser {
public:
    enum class State {
        RequestLine,
        Headers,
        Body,           // Content-Length 定长 body
        ChunkedBody,    // Transfer-Encoding: chunked
        Complete,
        Error,
    };

    HttpParser();

    // 解析 len 字节，返回消耗的字节数。
    // 返回 0 表示需要更多数据才能推进状态机。
    // 消耗的字节会被追加到 raw() 中。
    size_t parse(const char* data, size_t len);

    State state() const { return state_; }
    bool complete() const { return state_ == State::Complete; }
    bool has_error() const { return state_ == State::Error; }

    const HttpRequest& request() const { return request_; }
    const std::string& raw() const { return raw_; }

    void reset();

    void set_max_size(int max_size) { max_size_ = max_size; }

private:
    State state_ = State::RequestLine;
    HttpRequest request_;
    std::string raw_;           // 本条消息已消耗的原始字节
    std::string partial_;       // 未完成行 / 未消费 chunk 字节
    int max_size_ = 65536;

    // ---- Body 模式 ----
    bool is_chunked_ = false;
    int64_t content_length_ = -1;
    size_t body_read_ = 0;

    // ---- Chunked 子状态 ----
    enum class ChunkPhase { SizeLine, Data, Trailer, End };
    ChunkPhase chunk_phase_ = ChunkPhase::SizeLine;
    size_t current_chunk_size_ = 0;
    size_t current_chunk_read_ = 0;

    void process();
    void process_request_line();
    void process_headers();
    void process_body();
    void process_chunked();
    void finalize_headers();
    bool check_size_limit();
    bool extract_line(std::string& line);
};
