#include "gateway/http/request.h"

#include <algorithm>
#include <cctype>

static void to_lower_inplace(std::string& s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
}

bool request_keep_alive(const HttpRequest& req) {
    auto it = req.headers.find("connection");
    if (it != req.headers.end()) {
        std::string v = it->second;
        to_lower_inplace(v);
        if (v.find("keep-alive") != std::string::npos) return true;
    }

    if (req.version == "HTTP/1.1") return true;
    return false;
}
