#include "gateway/core/router.h"

#include <algorithm>

Router::Router(const std::vector<Route>& routes) : routes_(routes) {
    sort_routes();
}

std::optional<std::vector<Backend>> Router::match(const std::string& path) const {
    for (const auto& route : routes_) {
        if (path.size() >= route.prefix.size() &&
            path.compare(0, route.prefix.size(), route.prefix) == 0) {
            if (route.prefix.size() == path.size() || path[route.prefix.size()] == '/') {
                // 完全匹配，返回后端列表
                return route.backends;
            }
        }
    }
    return std::nullopt;
}

void Router::add_route(const Route& route) {
    remove_route(route.prefix);
    routes_.push_back(route);
    sort_routes();
}

bool Router::remove_route(const std::string& prefix) {
    auto it = std::remove_if(routes_.begin(), 
    routes_.end(), 
    [prefix](const Route& r) {
        return r.prefix == prefix;
    });

    if (it != routes_.end()) {
        routes_.erase(it, routes_.end());
        return true;
    }
    return false;
}
void Router::sort_routes() {
    std::sort(routes_.begin(), routes_.end(), [](const Route& a, const Route& b) {
        auto count_slashes = [](const std::string& s) {
            return std::count(s.begin(), s.end(), '/');
        };
        size_t a_slashes = count_slashes(a.prefix);
        size_t b_slashes = count_slashes(b.prefix);
        if (a_slashes != b_slashes) {
            return a_slashes > b_slashes;
        }
        return a.prefix.size() > b.prefix.size();
    });
}