#include "gateway/http/parser.h"
#include "gateway/utils/utils.h"  // parse_int_safe

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

// ============== 工具 ==============

static void to_lower_inplace(std::string& s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
}

// ============================================================
// 静态工具函数
// ============================================================

int parse_content_length(const char* header_data, size_t header_len) {
    const char* cl_pos = static_cast<const char*>(
        memmem(header_data, header_len, "Content-Length:", 15));
    if (!cl_pos) return -1;

    cl_pos += 15;
    while (cl_pos < header_data + header_len &&
           (*cl_pos == ' ' || *cl_pos == '\t')) {
        ++cl_pos;
    }

    const char* header_end = header_data + header_len;
    const char* cl_end = static_cast<const char*>(
        memmem(cl_pos, header_end - cl_pos, "\r\n", 2));
    if (!cl_end) cl_end = header_end;

    auto parsed = parse_int_safe(cl_pos, cl_end);
    if (parsed.has_value() && parsed.value() >= 0) {
        return parsed.value();
    }
    return -1;
}

static bool parse_request_line(const std::string& line, HttpRequest& req) {
    size_t pos1 = line.find(' ');
    if (pos1 == std::string::npos) return false;
    size_t pos2 = line.find(' ', pos1 + 1);
    if (pos2 == std::string::npos) return false;

    req.method = line.substr(0, pos1);
    req.path = line.substr(pos1 + 1, pos2 - pos1 - 1);
    req.version = line.substr(pos2 + 1);
    return !req.method.empty() && !req.path.empty() && !req.version.empty();
}

bool parse_http_request(const std::string& raw, HttpRequest& req) {
    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;

    req.body = raw.substr(header_end + 4);
    size_t pos = 0, line_start = 0;

    while (pos <= header_end) {
        if (raw[pos] == '\r' && pos + 1 <= header_end && raw[pos + 1] == '\n') {
            size_t line_len = pos - line_start;
            if (line_len > 0) {
                std::string line = raw.substr(line_start, line_len);
                if (req.method.empty()) {
                    if (!parse_request_line(line, req)) return false;
                } else {
                    size_t colon_pos = line.find(':');
                    if (colon_pos != std::string::npos) {
                        std::string key = line.substr(0, colon_pos);
                        to_lower_inplace(key);
                        size_t value_start = colon_pos + 1;
                        while (value_start < line.size() &&
                               (line[value_start] == ' ' || line[value_start] == '\t'))
                            ++value_start;
                        req.headers[key] = line.substr(value_start);
                    }
                }
            }
            pos += 2;
            line_start = pos;
        } else {
            ++pos;
        }
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
    partial_.clear();
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

bool HttpParser::extract_line(std::string& line) {
    size_t pos = partial_.find("\r\n");
    if (pos == std::string::npos) return false;
    line = partial_.substr(0, pos);
    partial_.erase(0, pos + 2);
    return true;
}

// ============== 主入口 ==============

size_t HttpParser::parse(const char* data, size_t len) {
    if (state_ == State::Error || state_ == State::Complete) return 0;

    // 全部追加到 partial_，由内部状态机消费
    size_t partial_before = partial_.size();
    partial_.append(data, len);
    process();
    size_t partial_after = partial_.size();
    size_t consumed = partial_before + len - partial_after;

    // 将消耗的字节追加到 raw_
    raw_.append(data, consumed);

    if (!check_size_limit()) return 0;

    return consumed;
}

// ============== 调度 ==============

void HttpParser::process() {
    while (state_ != State::Complete && state_ != State::Error) {
        switch (state_) {
        case State::RequestLine:  process_request_line(); break;
        case State::Headers:      process_headers();      break;
        case State::Body:         process_body();         break;
        case State::ChunkedBody:  process_chunked();      break;
        default: return;
        }
        // 行级解析：部分行则退出等更多数据
        if (state_ == State::RequestLine || state_ == State::Headers) return;
        // Body 解析：partial_ 为空则等更多数据
        if ((state_ == State::Body || state_ == State::ChunkedBody) &&
            partial_.empty() && state_ != State::Complete) return;
    }
}

// ============== RequestLine ==============

void HttpParser::process_request_line() {
    std::string line;
    if (!extract_line(line)) return;
    if (line.empty()) return;  // 跳过前导空行

    if (!parse_request_line(line, request_)) {
        state_ = State::Error;
        return;
    }
    state_ = State::Headers;
}

// ============== Headers ==============

void HttpParser::process_headers() {
    while (true) {
        if (partial_.size() < 2) return;

        // 空行 \r\n 表示头部结束
        if (partial_[0] == '\r' && partial_[1] == '\n') {
            partial_.erase(0, 2);
            finalize_headers();
            return;
        }

        std::string line;
        if (!extract_line(line)) return;
        if (line.empty()) continue;

        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;  // 宽松跳过无效行

        std::string key = line.substr(0, colon);
        to_lower_inplace(key);

        size_t vs = colon + 1;
        while (vs < line.size() && (line[vs] == ' ' || line[vs] == '\t')) ++vs;
        request_.headers[key] = line.substr(vs);
    }
}

void HttpParser::finalize_headers() {
    auto te = request_.headers.find("transfer-encoding");
    if (te != request_.headers.end()) {
        std::string v = te->second;
        to_lower_inplace(v);
        if (v.find("chunked") != std::string::npos)
            is_chunked_ = true;
    }

    if (is_chunked_) {
        state_ = State::ChunkedBody;
        chunk_phase_ = ChunkPhase::SizeLine;
        current_chunk_size_ = 0;
        current_chunk_read_ = 0;
    } else {
        // Content-Length 已在 raw_ 头部中
        const char* r = raw_.data();
        size_t rl = raw_.size();
        const char* he = static_cast<const char*>(memmem(r, rl, "\r\n\r\n", 4));
        if (he) {
            content_length_ = parse_content_length(r, he - r);
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
        const char* he = static_cast<const char*>(memmem(r, raw_.size(), "\r\n\r\n", 4));
        if (he) {
            request_.body = raw_.substr(he + 4 - r, static_cast<size_t>(content_length_));
        }
        state_ = State::Complete;
        return;
    }

    size_t consume = std::min(partial_.size(), remaining);
    partial_.erase(0, consume);
    body_read_ += consume;
}

// ============== ChunkedBody ==============

void HttpParser::process_chunked() {
    while (state_ == State::ChunkedBody) {
        switch (chunk_phase_) {
        case ChunkPhase::SizeLine: {
            size_t pos = partial_.find("\r\n");
            if (pos == std::string::npos) return;

            std::string hex = partial_.substr(0, pos);
            partial_.erase(0, pos + 2);

            size_t semi = hex.find(';');
            if (semi != std::string::npos) hex.resize(semi);

            errno = 0;
            char* endp = nullptr;
            long sz = std::strtol(hex.c_str(), &endp, 16);
            if (errno || endp != hex.c_str() + hex.size() || sz < 0) {
                state_ = State::Error;
                return;
            }
            current_chunk_size_ = static_cast<size_t>(sz);

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
                partial_.erase(0, 2);
                chunk_phase_ = ChunkPhase::SizeLine;
                break;
            }

            size_t consume = std::min(partial_.size(), remaining);
            partial_.erase(0, consume);
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
                    partial_.erase(0, 2);
                    chunk_phase_ = ChunkPhase::End;
                    break;
                }
                size_t pos = partial_.find("\r\n");
                if (pos == std::string::npos) return;
                partial_.erase(0, pos + 2);
            }
            break;
        }

        case ChunkPhase::End:
            state_ = State::Complete;
            return;
        }
    }
}
