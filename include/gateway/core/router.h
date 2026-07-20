#pragma once

#include "gateway/core/config.h"   // Backend, Route

#include <optional>
#include <string>
#include <vector>

// 路由管理器：将请求路径匹配到后端服务
//
// 匹配策略：最长前缀优先（longest prefix match）
//   例：注册 /api → backend_A, /api/user → backend_B
//       /api/order → backend_A（最长匹配 /api）
//       /api/user/profile → backend_B（最长匹配 /api/user）
//       /other → 无匹配
//

class Router {
public:
    Router() = default;
    ~Router() = default;

    // 从配置批量加载路由
    explicit Router(const std::vector<Route>& routes);

    // 按路径匹配后端，返回 std::nullopt 表示无匹配
    std::optional<std::vector<Backend>> match(const std::string& path) const;

    // 动态添加路由
    void add_route(const Route& route);

    // 按前缀移除路由
    bool remove_route(const std::string& prefix);

    size_t size() const { return routes_.size(); }

    bool empty() const { return routes_.empty(); }
private:
    // 按 prefix 长度降序排列，保证最长匹配优先
    std::vector<Route> routes_;

    void sort_routes();
};
