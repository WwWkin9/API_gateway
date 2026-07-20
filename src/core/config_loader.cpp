#include "gateway/core/config_loader.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ============== 内部错误状态 ==============

static std::string g_last_error;

const char* config_loader_last_error() {
    return g_last_error.c_str();
}

// ============== 递归下降 JSON 解析器 ==============

class JsonParser {
public:
    enum class TokenType {
        ObjectStart,   // {
        ObjectEnd,     // }
        ArrayStart,    // [
        ArrayEnd,      // ]
        String,        // "..."
        Number,        // 整数或浮点
        Boolean,       // true / false
        Colon,         // :
        Comma,         // ,
        Null,          // null
        End,
    };

    struct Token {
        TokenType type;
        std::string text;
    };

    explicit JsonParser(const std::string& input)
        : input_(input), pos_(0) {}

    // 词法分析：读取下一个 token
    void advance();

    // 在当前对象中按 key 查找值
    bool find_key(const std::string& key);

    // 读取值
    std::string read_string();
    int read_int(int default_val = 0);

    // 容器遍历
    bool enter_object();
    bool enter_array();
    void leave_container();

    // 跳过当前 token 对应的整个值（用于忽略未知键）
    void skip_value();

    // 当前 token 信息
    TokenType current_type() const { return current_.type; }
    const std::string& current_text() const { return current_.text; }

    bool has_error() const { return error_; }
    const std::string& error_msg() const { return error_msg_; }

private:
    std::string input_;
    size_t pos_;
    Token current_;
    bool error_ = false;
    std::string error_msg_;

    std::vector<TokenType> container_stack_;

    void skip_whitespace();
    void skip_comment();
    char peek() const;
    char consume();
    void set_error(const std::string& msg);
};

// ============== 字符级操作 ==============

char JsonParser::peek() const {
    return pos_ < input_.size() ? input_[pos_] : '\0';
}

char JsonParser::consume() {
    return pos_ < input_.size() ? input_[pos_++] : '\0';
}

void JsonParser::skip_whitespace() {
    while (pos_ < input_.size()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            consume();
        } else if (c == '/') {
            skip_comment();
        } else {
            break;
        }
    }
}

void JsonParser::skip_comment() {
    if (peek() == '/' && pos_ + 1 < input_.size()) {
        if (input_[pos_ + 1] == '/') {
            // 单行注释：跳过直到换行
            while (pos_ < input_.size() && peek() != '\n') {
                consume();
            }
        } else if (input_[pos_ + 1] == '*') {
            // 多行注释
            consume(); consume();  // 跳过
            while (pos_ + 1 < input_.size()) {
                if (peek() == '*' && input_[pos_ + 1] == '/') {
                    consume(); consume();
                    return;
                }
                consume();
            }
        }
    }
}

void JsonParser::set_error(const std::string& msg) {
    error_ = true;
    error_msg_ = msg;
}

// ============== 词法分析 ==============

void JsonParser::advance() {
    skip_whitespace();

    if (pos_ >= input_.size()) {
        current_.type = TokenType::End;
        current_.text = "";
        return;
    }

    char c = peek();

    switch (c) {
    case '{':
        consume();
        current_.type = TokenType::ObjectStart;
        current_.text = "{";
        break;
    case '}':
        consume();
        current_.type = TokenType::ObjectEnd;
        current_.text = "}";
        break;
    case '[':
        consume();
        current_.type = TokenType::ArrayStart;
        current_.text = "[";
        break;
    case ']':
        consume();
        current_.type = TokenType::ArrayEnd;
        current_.text = "]";
        break;
    case ':':
        consume();
        current_.type = TokenType::Colon;
        current_.text = ":";
        break;
    case ',':
        consume();
        current_.type = TokenType::Comma;
        current_.text = ",";
        break;
    case '"':
        {
            // 解析字符串
            consume();  // 跳过开头引号
            current_.text.clear();
            while (pos_ < input_.size()) {
                char ch = peek();
                if (ch == '"') {
                    consume();  // 跳过结尾引号
                    break;
                } else if (ch == '\\') {
                    consume();
                    if (pos_ < input_.size()) {
                        char escaped = consume();
                        switch (escaped) {
                        case '"':  current_.text += '"';  break;
                        case '\\': current_.text += '\\'; break;
                        case '/':  current_.text += '/';  break;
                        case 'n':  current_.text += '\n'; break;
                        case 't':  current_.text += '\t'; break;
                        case 'r':  current_.text += '\r'; break;
                        default:   current_.text += escaped; break;
                        }
                    }
                } else {
                    current_.text += consume();
                }
            }
            current_.type = TokenType::String;
        }
        break;
    default:
        // 数字、布尔值、null
        if (std::isdigit(c) || c == '-') {
            current_.text.clear();
            if (c == '-') current_.text += consume();
            while (pos_ < input_.size() && (std::isdigit(peek()) || peek() == '.')) {
                current_.text += consume();
            }
            current_.type = TokenType::Number;
        } else if (c == 't' || c == 'f') {
            // true / false
            current_.text.clear();
            while (pos_ < input_.size() && std::isalpha(peek())) {
                current_.text += consume();
            }
            current_.type = TokenType::Boolean;
        } else if (c == 'n') {
            // null
            current_.text.clear();
            while (pos_ < input_.size() && std::isalpha(peek())) {
                current_.text += consume();
            }
            current_.type = TokenType::Null;
        } else {
            set_error(std::string("Unexpected character: '") + c + "'");
        }
        break;
    }
}

// ============== 跳过值 ==============

void JsonParser::skip_value() {
    switch (current_.type) {
    case TokenType::ObjectStart:
        {
            int depth = 1;
            while (depth > 0) {
                advance();
                if (current_.type == TokenType::ObjectStart) depth++;
                else if (current_.type == TokenType::ObjectEnd) depth--;
                else if (current_.type == TokenType::End) break;
            }
        }
        break;
    case TokenType::ArrayStart:
        {
            int depth = 1;
            while (depth > 0) {
                advance();
                if (current_.type == TokenType::ArrayStart) depth++;
                else if (current_.type == TokenType::ArrayEnd) depth--;
                else if (current_.type == TokenType::End) break;
            }
        }
        break;
    default:
        // 简单值，不需要额外跳过
        break;
    }
}

// ============== find_key ==============

bool JsonParser::find_key(const std::string& key) {
    if (current_.type != TokenType::ObjectStart) {
        // 尝试进入对象
        advance();
        if (current_.type != TokenType::ObjectStart) {
            return false;
        }
    }

    // 推进到第一个 key
    advance();  // 期望 String 或 }

    while (current_.type == TokenType::String) {
        std::string cur_key = current_.text;

        advance();  // 期望 Colon

        if (cur_key == key) {
            advance();  // 推进到 value
            return true;
        }

        // 跳过当前 value
        skip_value();
        advance();  // 期望 Comma 或 }

        if (current_.type == TokenType::Comma) {
            advance();  // 推进到下一个 key
        }
    }

    return false;
}

// ============== 读取值 ==============

std::string JsonParser::read_string() {
    if (current_.type == TokenType::String) {
        return current_.text;
    }
    return "";
}

int JsonParser::read_int(int default_val) {
    if (current_.type == TokenType::Number) {
        errno = 0;
        long val = std::strtol(current_.text.c_str(), nullptr, 10);
        return (errno == 0) ? static_cast<int>(val) : default_val;
    }
    return default_val;
}

// ============== 容器遍历 ==============

bool JsonParser::enter_object() {
    if (current_.type != TokenType::ObjectStart) {
        advance();
        if (current_.type != TokenType::ObjectStart) {
            return false;
        }
    }
    container_stack_.push_back(TokenType::ObjectStart);
    advance();  // 推进到第一个 key 或 }
    return true;
}

bool JsonParser::enter_array() {
    if (current_.type != TokenType::ArrayStart) {
        advance();
        if (current_.type != TokenType::ArrayStart) {
            return false;
        }
    }
    container_stack_.push_back(TokenType::ArrayStart);
    advance();  // 推进到第一个元素或 ]
    return true;
}

void JsonParser::leave_container() {
    if (container_stack_.empty()) return;
    container_stack_.pop_back();
}

// ============== 配置解析 ==============

static std::optional<GatewayConfig> parse_config(const std::string& json) {
    JsonParser parser(json);

    // 读取根对象 {
    parser.advance();
    if (parser.current_type() != JsonParser::TokenType::ObjectStart) {
        g_last_error = "Expected root object '{'";
        return std::nullopt;
    }

    GatewayConfig cfg;

    // 遍历顶层键值对
    parser.advance();  // 第一个 key 或 }

    while (parser.current_type() == JsonParser::TokenType::String) {
        std::string key = parser.current_text();

        parser.advance();  // Colon
        parser.advance();  // value

        if (key == "listen_port") {
            cfg.listen_port = parser.read_int(cfg.listen_port);
        } else if (key == "keep_alive_timeout_sec") {
            cfg.keep_alive_timeout_sec = parser.read_int(cfg.keep_alive_timeout_sec);
        } else if (key == "thread_count") {
            cfg.thread_count = parser.read_int(cfg.thread_count);
        } else if (key == "backend_timeout_ms") {
            cfg.backend_timeout_ms = parser.read_int(cfg.backend_timeout_ms);
        } else if (key == "max_request_size") {
            cfg.max_request_size = parser.read_int(cfg.max_request_size);
        } else if (key == "max_epoll_events") {
            cfg.max_epoll_events = parser.read_int(cfg.max_epoll_events);
        } else if (key == "idle_cleanup_interval_sec") {
            cfg.idle_cleanup_interval_sec = parser.read_int(cfg.idle_cleanup_interval_sec);
        } else if (key == "pool_max_idle_per_host") {
            cfg.pool_max_idle_per_host = parser.read_int(cfg.pool_max_idle_per_host);
        } else if (key == "pool_idle_timeout_sec") {
            cfg.pool_idle_timeout_sec = parser.read_int(cfg.pool_idle_timeout_sec);
        } else if (key == "routes") {
            // 解析路由数组
            if (parser.current_type() != JsonParser::TokenType::ArrayStart) {
                g_last_error = "Expected array for 'routes'";
                return std::nullopt;
            }

            parser.advance();  // 第一个 route 对象或 ]

            while (parser.current_type() == JsonParser::TokenType::ObjectStart) {
                Route route;

                parser.advance();  // 第一个 key

                while (parser.current_type() == JsonParser::TokenType::String) {
                    std::string route_key = parser.current_text();

                    parser.advance();  // Colon
                    parser.advance();  // value

                    if (route_key == "prefix") {
                        route.prefix = parser.read_string();
                    } else if (route_key == "backends") {
                        // 解析后端数组
                        if (parser.current_type() != JsonParser::TokenType::ArrayStart) {
                            g_last_error = "Expected array for 'backends'";
                            return std::nullopt;
                        }

                        parser.advance();  // 第一个 backend 或 ]

                        while (parser.current_type() == JsonParser::TokenType::ObjectStart) {
                            Backend backend;

                            parser.advance();  // 第一个 key

                            while (parser.current_type() == JsonParser::TokenType::String) {
                                std::string bk_key = parser.current_text();

                                parser.advance();  // Colon
                                parser.advance();  // value

                                if (bk_key == "host") {
                                    backend.host = parser.read_string();
                                } else if (bk_key == "port") {
                                    backend.port = parser.read_int(0);
                                } else {
                                    parser.skip_value();
                                }

                                parser.advance();  // Comma 或 }
                            }

                            if (!backend.host.empty() && backend.port > 0) {
                                route.backends.push_back(backend);
                            }

                            // current_type 应该是 Comma 或 }
                            if (parser.current_type() == JsonParser::TokenType::Comma) {
                                parser.advance();  // 下一个 backend
                            }
                        }
                        // current_type 现在是 ArrayEnd
                    } else {
                        parser.skip_value();
                    }

                    parser.advance();  // Comma 或 }
                }

                if (!route.prefix.empty() && !route.backends.empty()) {
                    cfg.routes.push_back(route);
                }

                // current_type 应该是 Comma 或 }
                if (parser.current_type() == JsonParser::TokenType::Comma) {
                    parser.advance();  // 下一个 route
                }
            }
            // current_type 现在是 ArrayEnd
        } else {
            // 未知键，跳过值
            parser.skip_value();
        }

        parser.advance();  // Comma 或 }

        if (parser.current_type() != JsonParser::TokenType::Comma &&
            parser.current_type() != JsonParser::TokenType::ObjectEnd) {
            break;
        }
    }

    if (parser.has_error()) {
        g_last_error = parser.error_msg();
        return std::nullopt;
    }

    return cfg;
}

// ============== 公开接口 ==============

std::optional<GatewayConfig> ConfigLoader::load(const std::string& filepath) {
    g_last_error.clear();

    FILE* fp = std::fopen(filepath.c_str(), "r");
    if (!fp) {
        g_last_error = std::string("Failed to open file: ") + filepath;
        return std::nullopt;
    }

    // 读取整个文件
    std::fseek(fp, 0, SEEK_END);
    long fsize = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);

    if (fsize <= 0) {
        std::fclose(fp);
        g_last_error = "Empty or unreadable file";
        return std::nullopt;
    }

    std::string content(static_cast<size_t>(fsize), '\0');
    size_t read = std::fread(&content[0], 1, static_cast<size_t>(fsize), fp);
    std::fclose(fp);

    if (read != static_cast<size_t>(fsize)) {
        g_last_error = "Failed to read file content";
        return std::nullopt;
    }

    return load_json(content);
}

std::optional<GatewayConfig> ConfigLoader::load_json(const std::string& json) {
    g_last_error.clear();

    auto cfg = parse_config(json);
    if (!cfg.has_value() && g_last_error.empty()) {
        g_last_error = "Unknown parse error";
    }

    return cfg;
}
