#pragma once

#include "gateway/http/request.h"  // HttpRequest

#include <cstddef>

// HTTP 解析器：底层解析原语，供 Connection、Gateway 等模块使用
//
// 用法：
//   int cl = parse_content_length(header_data, header_len);
//   HttpRequest req;
//   parse_http_request(raw, req);

// 从原始 HTTP 头部数据中提取 Content-Length 值
// 返回 -1 表示头部中没有 Content-Length 或解析失败
int parse_content_length(const char* header_data, size_t header_len);

// 解析 HTTP 请求报文
// 成功返回 true，失败返回 false（此时 req 状态未定义）
bool parse_http_request(const std::string& raw, HttpRequest& req);
