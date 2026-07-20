#include "gateway/http/request.h"

#include <string_view>

static inline char ascii_to_lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c);
}

static bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_to_lower(static_cast<unsigned char>(a[i])) !=
            ascii_to_lower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool request_keep_alive(const HttpRequest& req) {
    auto it = req.headers.find("connection");
    if (it != req.headers.end()) {
        std::string_view v(it->second);
        for (size_t i = 0; i + 10 <= v.size(); ++i) {
            if (iequal(v.substr(i, 10), "keep-alive")) return true;
        }
    }

    if (req.version == "HTTP/1.1") return true;
    return false;
}
