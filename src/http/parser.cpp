#include "gateway/http/parser.h"
#include "gateway/utils/utils.h"  // parse_int_safe
#include "gateway/logger/logger.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string_view>

// ============== 工具 ==============

static inline char ascii_to_lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c);
}

static void to_lower_inplace(std::string& s) {
    for (char& c : s) {
        c = ascii_to_lower(static_cast<unsigned char>(c));
    }
}

static bool str_iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_to_lower(static_cast<unsigned char>(a[i])) !=
            ascii_to_lower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// ============================================================
// 静态工具函数
// ============================================================

static const char* header_find_icase(const char* data, size_t len,
                                     std::string_view name) {
    const char* p = data;
    const char* end = data + len;
    while (p < end) {
        const char* line_end = static_cast<const char*>(::memmem(p, end - p, "\r\n", 2));
        if (!line_end) line_end = end;

        if (static_cast<size_t>(line_end - p) > name.size() &&
            p[name.size()] == ':' &&
            str_iequal(std::string_view(p, name.size()), name)) {
            return p;
        }
        if (line_end == end) break;
        p = line_end + 2;
    }
    return nullptr;
}

int parse_content_length(const char* header_data, size_t header_len) {
    const char* cl_pos = header_find_icase(header_data, header_len, "Content-Length");
    if (!cl_pos) return -1;

    const char* header_end = header_data + header_len;
    cl_pos += 15;  // "Content-Length:"
    while (cl_pos < header_end && (*cl_pos == ' ' || *cl_pos == '\t')) {
        ++cl_pos;
    }

    const char* cl_end = static_cast<const char*>(::memmem(cl_pos, header_end - cl_pos, "\r\n", 2));
    if (!cl_end) cl_end = header_end;

    auto parsed = parse_int_safe(cl_pos, cl_end);
    if (parsed.has_value() && parsed.value() >= 0) {
        return parsed.value();
    }
    return -1;
}

static bool parse_request_line(std::string_view line, HttpRequest& req) {
    size_t pos1 = line.find(' ');
    if (pos1 == std::string_view::npos) return false;
    size_t pos2 = line.find(' ', pos1 + 1);
    if (pos2 == std::string_view::npos) return false;

    req.method.assign(line.data(), pos1);
    req.path.assign(line.data() + pos1 + 1, pos2 - pos1 - 1);
    req.version.assign(line.data() + pos2 + 1, line.size() - pos2 - 1);
    return !req.method.empty() && !req.path.empty() && !req.version.empty();
}

bool parse_http_request(const std::string& raw, HttpRequest& req) {
    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;

    req.body.assign(raw.data() + header_end + 4, raw.size() - header_end - 4);

    size_t line_start = 0;
    while (line_start < header_end) {
        size_t line_end = raw.find("\r\n", line_start);
        if (line_end == std::string::npos || line_end > header_end) break;

        std::string_view line(raw.data() + line_start, line_end - line_start);
        if (!line.empty()) {
            if (req.method.empty()) {
                if (!parse_request_line(line, req)) return false;
            } else {
                size_t colon_pos = line.find(':');
                if (colon_pos != std::string_view::npos) {
                    std::string key(line.data(), colon_pos);
                    to_lower_inplace(key);
                    size_t value_start = colon_pos + 1;
                    while (value_start < line.size() &&
                           (line[value_start] == ' ' || line[value_start] == '\t')) {
                        ++value_start;
                    }
                    req.headers[std::move(key)].assign(
                        line.data() + value_start, line.size() - value_start);
                }
            }
        }
        line_start = line_end + 2;
    }
    return !req.method.empty() && !req.path.empty() && !req.version.empty();
}

// ============================================================
// HttpParser 增量状态机
// ============================================================

HttpParser::HttpParser() {
    raw_.reserve(4096);
    partial_.reserve(256);
}

void HttpParser::reset() {
    state_ = State::RequestLine;
    request_ = HttpRequest{};
    raw_.clear();
    // 注意：不清理 partial_，保留管道请求的剩余数据
    is_chunked_ = false;
    content_length_ = -1;
    body_read_ = 0;
    chunk_phase_ = ChunkPhase::SizeLine;
    current_chunk_size_ = 0;
    current_chunk_read_ = 0;
}

bool HttpParser::check_size_limit() {
    if (raw_.size() > static_cast<size_t>(max_size_)) {
        state_ = State::Error;
        return false;
    }
    return true;
}

// 从 partial_ 开头消费 n 字节，追加到 raw_，并把剩余数据前移
void HttpParser::consume_partial(size_t n) {
    if (n == 0) return;
    raw_.append(partial_.data(), n);
    size_t remaining = partial_.size() - n;
    if (remaining > 0) {
        std::memmove(partial_.data(), partial_.data() + n, remaining);
    }
    partial_.resize(remaining);
}

bool HttpParser::extract_line(std::string_view& line) {
    const char* data = partial_.data();
    size_t len = partial_.size();
    const char* cr = static_cast<const char*>(::memchr(data, '\r', len));
    if (!cr || cr + 1 >= data + len || cr[1] != '\n') return false;

    size_t line_len = cr - data;
    line = std::string_view(data, line_len);
    // 不在此处 consume，由调用方在 line 使用完毕后再消费
    return true;
}

void HttpParser::consume_line(const std::string_view& line) {
    consume_partial(line.size() + 2);
}

// ============== 主入口 ==============

size_t HttpParser::parse(const char* data, size_t len) {
    if (state_ == State::Error || state_ == State::Complete) return 0;

    size_t old_pending = partial_.size();
    size_t old_raw_size = raw_.size();
    partial_.append(data, len);

    process();

    if (!check_size_limit()) return 0;

    // 有输入数据时：所有数据已复制到 partial_，返回 len
    if (len > 0) return len;

    // 无输入数据时（处理 partial_ 中残留的管道数据）：返回实际解析进度
    // 返回 0 表示无法继续解析，调用方应等待更多数据而非循环重试
    size_t consumed = raw_.size() - old_raw_size;
    return consumed;
}

// ============== 调度 ==============

void HttpParser::process() {
    while (state_ != State::Complete && state_ != State::Error) {
        switch (state_) {
        case State::RequestLine:
            process_request_line();
            if (state_ == State::Headers) continue;
            break;
        case State::Headers:      process_headers();      break;
        case State::Body:         process_body();         break;
        case State::ChunkedBody:  process_chunked();      break;
        default: return;
        }
        if (state_ == State::RequestLine || state_ == State::Headers) {
            return;
        }
        if ((state_ == State::Body || state_ == State::ChunkedBody) &&
            partial_.empty() && state_ != State::Complete) return;
    }
}

// ============== RequestLine ==============

void HttpParser::process_request_line() {
    std::string_view line;
    if (!extract_line(line)) return;
    if (line.empty()) {
        consume_line(line);  // 跳过前导空行
        return;
    }

    if (!parse_request_line(line, request_)) {
        state_ = State::Error;
        return;
    }
    consume_line(line);
    state_ = State::Headers;
}

// ============== Headers ==============

void HttpParser::process_headers() {
    while (true) {
        if (partial_.size() < 2) {
            return;
        }

        if (partial_[0] == '\r' && partial_[1] == '\n') {
            consume_partial(2);
            finalize_headers();
            return;
        }

        std::string_view line;
        if (!extract_line(line)) {
            return;
        }
        if (line.empty()) {
            consume_line(line);
            continue;
        }

        size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            consume_line(line);
            continue;
        }

        std::string key(line.data(), colon);
        to_lower_inplace(key);

        size_t vs = colon + 1;
        while (vs < line.size() && (line[vs] == ' ' || line[vs] == '\t')) ++vs;
        request_.headers[std::move(key)].assign(line.data() + vs, line.size() - vs);

        consume_line(line);
    }
}

void HttpParser::finalize_headers() {
    auto te = request_.headers.find("transfer-encoding");
    if (te != request_.headers.end()) {
        std::string_view v(te->second);
        for (size_t i = 0; i + 7 <= v.size(); ++i) {
            if (str_iequal(v.substr(i, 7), "chunked")) {
                is_chunked_ = true;
                break;
            }
        }
    }

    if (is_chunked_) {
        state_ = State::ChunkedBody;
        chunk_phase_ = ChunkPhase::SizeLine;
        current_chunk_size_ = 0;
        current_chunk_read_ = 0;
    } else {
        auto cl = request_.headers.find("content-length");
        if (cl != request_.headers.end()) {
            auto parsed = parse_int_safe(cl->second.c_str(),
                                         cl->second.c_str() + cl->second.size());
            if (parsed.has_value() && parsed.value() >= 0) {
                content_length_ = parsed.value();
            }
        }

        if (content_length_ > 0) {
            state_ = State::Body;
            body_read_ = 0;
        } else {
            state_ = State::Complete;
        }
    }
}

// ============== Body (Content-Length) ==============

void HttpParser::process_body() {
    if (content_length_ <= 0) {
        state_ = State::Complete;
        return;
    }

    size_t remaining = static_cast<size_t>(content_length_) - body_read_;
    if (remaining == 0) {
        // 从 raw_ 提取 body
        const char* r = raw_.data();
        const char* he = static_cast<const char*>(::memmem(r, raw_.size(), "\r\n\r\n", 4));
        if (he) {
            request_.body.assign(he + 4, static_cast<size_t>(content_length_));
        }
        state_ = State::Complete;
        return;
    }

    size_t consume = std::min(partial_.size(), remaining);
    consume_partial(consume);
    body_read_ += consume;

    // 如果消费完整个 body，立即提取并标记完成
    if (body_read_ >= static_cast<size_t>(content_length_)) {
        const char* r = raw_.data();
        const char* he = static_cast<const char*>(::memmem(r, raw_.size(), "\r\n\r\n", 4));
        if (he) {
            request_.body.assign(he + 4, static_cast<size_t>(content_length_));
        }
        state_ = State::Complete;
    }
}

static bool parse_hex(std::string_view s, size_t& out) {
    if (s.empty()) return false;
    size_t value = 0;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        size_t digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        value = (value << 4) | digit;
    }
    out = value;
    return true;
}

// ============== ChunkedBody ==============

void HttpParser::process_chunked() {
    while (state_ == State::ChunkedBody) {
        switch (chunk_phase_) {
        case ChunkPhase::SizeLine: {
            const char* data = partial_.data();
            size_t len = partial_.size();
            const char* cr = static_cast<const char*>(::memchr(data, '\r', len));
            if (!cr || cr + 1 >= data + len || cr[1] != '\n') return;

            size_t line_len = cr - data;
            std::string_view hex(data, line_len);
            size_t semi = hex.find(';');
            if (semi != std::string_view::npos) hex = std::string_view(hex.data(), semi);

            size_t sz = 0;
            if (!parse_hex(hex, sz)) {
                state_ = State::Error;
                return;
            }
            current_chunk_size_ = sz;
            consume_partial(line_len + 2);

            if (current_chunk_size_ == 0) {
                chunk_phase_ = ChunkPhase::Trailer;
            } else {
                if (!check_size_limit()) return;
                chunk_phase_ = ChunkPhase::Data;
                current_chunk_read_ = 0;
            }
            break;
        }

        case ChunkPhase::Data: {
            size_t remaining = current_chunk_size_ - current_chunk_read_;
            if (remaining == 0) {
                if (partial_.size() < 2) return;
                if (partial_[0] != '\r' || partial_[1] != '\n') {
                    state_ = State::Error;
                    return;
                }
                consume_partial(2);
                chunk_phase_ = ChunkPhase::SizeLine;
                break;
            }

            size_t consume = std::min(partial_.size(), remaining);
            consume_partial(consume);
            current_chunk_read_ += consume;

            if (!check_size_limit()) return;
            if (current_chunk_read_ >= current_chunk_size_)
                continue;  // 回到 Data 处理尾部 \r\n
            return;
        }

        case ChunkPhase::Trailer: {
            while (true) {
                if (partial_.size() < 2) return;
                if (partial_[0] == '\r' && partial_[1] == '\n') {
                    consume_partial(2);
                    chunk_phase_ = ChunkPhase::End;
                    break;
                }
                const char* data = partial_.data();
                size_t len = partial_.size();
                const char* cr = static_cast<const char*>(::memchr(data, '\r', len));
                if (!cr || cr + 1 >= data + len || cr[1] != '\n') return;
                consume_partial(static_cast<size_t>(cr - data) + 2);
            }
            break;
        }

        case ChunkPhase::End:
            state_ = State::Complete;
            return;
        }
    }
}
