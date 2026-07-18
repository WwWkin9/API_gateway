# C++ API Gateway

一个基于 **Reactor + 线程池** 架构的HTTP 反向代理网关，使用 epoll 实现非阻塞 I/O，支持路由转发、过滤器链、连接管理和 Keep-Alive。

## 项目结构

```
API_gateway/
├── CMakeLists.txt
├── README.md
├── docs/
│   ├── architecture.md
│   └── benchmark.md
├── include/gateway/
│   ├── core/
│   │   ├── gateway.h               # 网关核心类
│   │   ├── router.h                # 路由管理
│   │   └── config.h                # 配置结构体与路由定义
│   ├── net/
│   │   ├── event_loop.h            # 事件循环（epoll）
│   │   ├── tcp_server.h            # TCP 服务器
│   │   ├── connection.h            # 连接管理
│   │   ├── buffer.h                # 缓冲区
│   │   └── socket.h                # Socket 工具
│   ├── http/
│   │   ├── request.h               # HTTP 请求解析
│   │   ├── response.h              # HTTP 响应构造
│   │   └── parser.h                # HTTP 解析器
│   ├── proxy/
│   │   ├── proxy.h                 # 反向代理
│   │   ├── backend.h               # 后端服务定义
│   │   ├── backend_pool.h          # 后端连接池
│   │   └── load_balancer.h         # 负载均衡器
│   ├── filter/
│   │   ├── filter.h                # 过滤器基类与上下文
│   │   ├── logging_filter.h        # 日志过滤器
│   │   └── request_id_filter.h     # 请求 ID 过滤器
│   ├── timer/
│   │   └── timer.h                 # 定时器
│   ├── logger/
│   │   ├── logger.h                # 日志器
│   │   └── async_logger.h          # 异步日志器
│   └── utils/
│       ├── utils.h                 # 工具函数
│       └── thread_pool.h           # 线程池
├── src/
│   ├── main.cpp                    # 入口，组装过滤器并启动网关
│   ├── core/
│   │   ├── gateway.cpp             # 网关核心逻辑（epoll、转发、过滤）
│   │   ├── router.cpp              # 路由匹配实现
│   │   └── config.cpp              # 默认配置与路由表
│   ├── net/
│   │   ├── event_loop.cpp          # 事件循环实现
│   │   ├── tcp_server.cpp          # TCP 服务器实现
│   │   ├── connection.cpp          # 连接管理实现
│   │   └── buffer.cpp              # 缓冲区实现
│   ├── http/
│   │   ├── parser.cpp              # HTTP 解析器实现
│   │   ├── request.cpp             # HTTP 请求解析实现
│   │   └── response.cpp            # HTTP 响应构造实现
│   ├── proxy/
│   │   ├── proxy.cpp               # 反向代理实现
│   │   ├── backend_pool.cpp        # 后端连接池实现
│   │   └── load_balancer.cpp       # 负载均衡器实现
│   ├── filter/
│   │   ├── logging_filter.cpp      # 日志过滤器实现
│   │   └── request_id_filter.cpp   # 请求 ID 过滤器实现
│   ├── timer/
│   ├── logger/
│   └── utils/
│       ├── utils.cpp               # 工具函数实现
│       └── thread_pool.cpp         # 线程池实现
├── tests/
├── benchmark/
├── backend/
│   ├── user_server.cpp             # 模拟用户服务（端口 9001）
│   └── order_server.cpp            # 模拟订单服务（端口 9002）
└── examples/
```

## 架构设计

```
                     +--------------+
                     |    Client    |
                     +------+-------+
                            |
                     +------v-------+
                     |   Gateway    |
                     |   (epoll)    |
                     +------+-------+
                            |
              +-------------+-------------+
              v                           v
    +-----------------+         +-----------------+
    |  Filter Chain   |         |     Route       |
    |  +-----------+  |         |  /api/user  --> |---> user_server :9001
    |  |RequestId  |  |         |  /api/order --> |---> order_server :9002
    |  |Filter     |  |         +-----------------+
    |  +-----------+  |
    |  +-----------+  |
    |  | Logging   |  |
    |  | Filter    |  |
    |  +-----------+  |
    +-----------------+
```

- **Reactor 线程**：使用 epoll（边缘触发 EPOLLET）监听客户端连接和请求，接收完整 HTTP 请求后交给线程池
- **线程池**：工作线程并行处理请求解析、过滤器链执行、路由转发
- **过滤器链**：请求阶段按注册顺序执行，响应阶段逆序执行，任一过滤器返回 `false` 即中断处理
- **非阻塞转发**：到后端服务的连接、发送、接收均使用非阻塞 I/O + poll 超时控制

## 功能特性

### 路由转发

根据请求路径前缀匹配，将请求反向代理到对应的后端服务。路由表在 `config.cpp` 中配置：

| 路径前缀      | 后端地址       | 服务         |
|--------------|---------------|-------------|
| `/api/user`  | 127.0.0.1:9001 | 用户服务     |
| `/api/order` | 127.0.0.1:9002 | 订单服务     |

无匹配路由时返回 **404**，后端不可达时返回 **502**。

### 过滤器链

过滤器在请求处理的不同阶段介入，支持灵活扩展：

| 阶段         | 说明                                                    |
|-------------|--------------------------------------------------------|
| `on_request`  | 请求到达后、路由前调用；返回 `false` 中断处理链，返回 400 |
| `on_response` | 后端响应后、发送客户端前调用；可修改响应头                |

内置过滤器：

- **RequestIdFilter**：从请求头 `X-Request-Id` 透传或自动生成 8 位 hex ID，注入响应头
- **LoggingFilter**：记录请求方法、路径、响应状态码和耗时

### 连接管理

- 支持 HTTP Keep-Alive，空闲超时可配置（默认 10 秒）
- 定时清理空闲连接（默认每 5 秒扫描一次）
- 请求处理完毕后立即关闭该连接的 epoll 监听

### 错误处理

| 场景              | 响应码 |
|------------------|--------|
| 请求解析失败      | 400    |
| 无匹配路由        | 404    |
| 后端连接/转发失败 | 502    |

## 构建与运行

### 前置要求

- **C++17** 编译器（GCC 8+ 或 Clang 7+）
- **CMake** >= 3.10
- **Linux**（依赖 epoll）

### 编译

```bash
cd API_gateway
mkdir -p build && cd build
cmake ..
make
```

编译产物：

- `build/gateway` — 网关主程序
- `build/user_server` — 模拟用户服务
- `build/order_server` — 模拟订单服务

### 运行

先启动后端模拟服务，再启动网关：

```bash
# 终端 1：启动用户服务（监听 9001）
./build/user_server

# 终端 2：启动订单服务（监听 9002）
./build/order_server

# 终端 3：启动网关（监听 8080）
./build/gateway
```

### 测试

```bash
# 请求用户服务
curl -v http://localhost:8080/api/user

# 请求订单服务
curl http://localhost:8080/api/order -d '{"item":"book"}'

# 带自定义 Request-Id
curl -H "X-Request-Id: my-id-123" http://localhost:8080/api/user

# 测试 Keep-Alive（复用连接发送两次请求）
curl -v -H "Connection: keep-alive" http://localhost:8080/api/user http://localhost:8080/api/order

# 无匹配路由（返回 404）
curl -v http://localhost:8080/api/unknown
```

## 配置说明

网关配置通过 `GatewayConfig` 结构体定义，在 `load_default_config()` 中设置默认值：

| 参数                       | 默认值  | 说明                        |
|---------------------------|--------|-----------------------------|
| `listen_port`             | 8080   | 网关监听端口                  |
| `keep_alive_timeout_sec`  | 10     | Keep-Alive 空闲超时（秒）     |
| `thread_count`            | 4      | 线程池工作线程数              |
| `backend_timeout_ms`      | 3000   | 后端转发超时（毫秒）          |
| `max_request_size`        | 65536  | 请求体最大字节数（64 KB）     |
| `max_epoll_events`        | 512    | 每轮 epoll_wait 最大事件数    |
| `idle_cleanup_interval_sec` | 5    | 空闲连接清理间隔（秒）        |

## 扩展指南

### 添加新路由

编辑 `src/core/config.cpp`，在 `load_default_config()` 中添加：

```cpp
cfg.routes.push_back({"/api/product", {"127.0.0.1", 9003}});
```

### 自定义过滤器

1. 新建头文件 `include/gateway/filter/xxx_filter.h`，继承 `Filter` 类
2. 重写 `on_request()` 和/或 `on_response()` 方法
3. 在 `main.cpp` 中注册：

```cpp
gw.add_filter(std::make_unique<YourFilter>());
```

### 添加新后端服务

参考 `backend/` 目录下的示例，创建新的 TCP 服务，并在 CMakeLists.txt 中添加：

```cmake
add_executable(product_server backend/product_server.cpp)
```

## 技术要点

- **I/O 模型**：Reactor 单线程 + 边缘触发 epoll + 非阻塞 socket
- **并发模型**：请求解析与转发在独立线程池中执行，Reactor 线程只负责 I/O 事件分发
- **内存管理**：使用 `std::unique_ptr` 管理过滤器所有权，RAII 管理 socket 生命周期
- **超时控制**：后端 connect/send/recv 均设置超时，防止工作线程被阻塞连接耗尽

## 压测

推荐使用 `wrk` 做网关压测。仓库已提供可复用脚本：

- `scripts/wrk_mixed.lua`：50% `GET /api/user` + 50% `POST /api/order`
- `scripts/bench_gateway.sh`：自动执行 warmup + 阶梯压测，并输出 CSV 汇总

### 安装 wrk

```bash
sudo apt-get update
sudo apt-get install -y wrk
```

### 启动被测服务

```bash
# 终端 1
./build/user_server

# 终端 2
./build/order_server

# 终端 3
./build/gateway
```

### 一键压测

```bash
./scripts/bench_gateway.sh
```

默认行为：

- warmup：`50` 连接，`10s`
- 阶梯压测：连接数 `50 100 200 400 800`
- 每档压测时长：`30s`
- 线程数：`8`
- 结果输出：`bench_results/summary_*.csv` 与 `bench_results/run_*.log`

### 自定义参数

```bash
THREADS=12 DURATION=60s CONNECTIONS_LIST="100 200 400 800 1200" ./scripts/bench_gateway.sh http://127.0.0.1:8080
```

可用环境变量：

- `THREADS`：wrk 线程数
- `DURATION`：每档压测时长
- `WARMUP_DURATION`：预热时长
- `CONNECTIONS_LIST`：阶梯连接数列表
- `OUTPUT_DIR`：结果目录
- `LUA_SCRIPT`：自定义 wrk Lua 脚本

### 指标解读建议

- 关注 `requests_per_sec`、`p99`、`non_2xx_3xx`、`socket_errors`
- 若 `p99` 随连接数上升明显恶化且错误率升高，通常已到容量拐点
- 若网关压测结果偏低，优先排查后端服务是否先成为瓶颈

### 多次结果对比

仓库提供了结果对比脚本 `scripts/compare_bench.sh`，用于对最近多次压测结果做横向比较。

默认行为：

- 输入目录：`bench_results`
- 选择最近 `5` 份 `summary_*.csv`
- 只比较 `staircase` 阶段
- 输出文件：`bench_results/compare_*.csv`

执行：

```bash
./scripts/compare_bench.sh
```

自定义示例：

```bash
LATEST_N=10 PHASE=staircase INPUT_DIR=bench_results OUTPUT_DIR=bench_results ./scripts/compare_bench.sh
```

输出表结构：

- 每一行是一个连接数（`connections`）
- 每次运行会展开为 4 列：`rps`、`p99`、`non_2xx_3xx`、`socket_errors`
