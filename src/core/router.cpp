#include "gateway/core/router.h"

#include <algorithm>
#include "gateway/logger/logger.h"

Router::Router(const std::vector<Route>& routes) : routes_(routes) {
    for (const auto& r : routes_) {
        LOG_INFO("Router: added route prefix=[%s] backends=%zu", r.prefix.c_str(), r.backends.size());
    }
    sort_routes();
}

std::optional<std::vector<Backend>> Router::match(const std::string& path) const {
    for (const auto& route : routes_) {
        if (path.size() >= route.prefix.size() &&
            path.compare(0, route.prefix.size(), route.prefix) == 0) {
            if (route.prefix.size() == path.size() || path[route.prefix.size()] == '/') {
                return route.backends;
            }
        }
    }
    return std::nullopt;
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