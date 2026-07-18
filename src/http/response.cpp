#include "gateway/http/response.h"

// 响应序列化
std::string HttpResponse::to_string() const {
    size_t estimate_size = 128 + body.size();
    for (const auto& [k, v] : headers) {
        estimate_size += k.size() + v.size() + 4;
    }

    std::string result;
    result.reserve(estimate_size);

    result += "HTTP/1.1 ";
    result += std::to_string(status_code);
    result += " ";
    result += status_text;
    result += "\r\n";

    bool has_cl = false;
    bool has_conn = false;

    for (const auto& [k, v] : headers) {
        if (k == "Content-Length") has_cl = true;
        if (k == "Connection") has_conn = true;
        result += k;
        result += ": ";
        result += v;
        result += "\r\n";
    }

    if (!has_cl) {
        result += "Content-Length: ";
        result += std::to_string(body.size());
        result += "\r\n";
    }
    if (!has_conn) {
        result += "Connection: close\r\n";
    }

    result += "\r\n";
    result += body;
    return result;
}

static HttpResponse make_common(int code, const std::string& text, bool keep_alive) {
    HttpResponse resp;
    resp.status_code = code;
    resp.status_text = text;
    resp.headers["Connection"] = keep_alive ? "keep-alive" : "close";
    return resp;
}

HttpResponse make_400(bool keep_alive) {
    auto r = make_common(400, "Bad Request", keep_alive);
    r.body = "Bad Request";
    return r;
}

HttpResponse make_404(bool keep_alive) {
    auto r = make_common(404, "Not Found", keep_alive);
    r.body = "Not Found";
    return r;
}

HttpResponse make_502(bool keep_alive) {
    auto r = make_common(502, "Bad Gateway", keep_alive);
    r.body = "Bad Gateway";
    return r;
}