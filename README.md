# C++ API Gateway

> 基于 **Reactor + Thread Pool** 架构的高性能 HTTP 反向代理网关，零外部依赖，从零手写实现。

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/CMake-3.10%2B-green)](https://cmake.org/)
[![Platform](https://img.shields.io/badge/Platform-Linux-lightgrey)](https://kernel.org/)
[![License](https://img.shields.io/badge/License-MIT-yellow)](LICENSE)

---

## 目录

- [项目概述](#项目概述)
- [功能特性](#功能特性)
- [技术栈选型](#技术栈选型)
- [架构设计](#架构设计)
- [项目结构](#项目结构)
- [快速开始](#快速开始)
  - [构建编译](#构建编译)
  - [启动运行](#启动运行)
  - [配置说明](#配置说明)
- [使用指南](#使用指南)
  - [API 端点](#api-端点)
  - [命令行参数](#命令行参数)
  - [压测工具](#压测工具)
- [核心模块详解](#核心模块详解)
- [核心技术难点与解决方案](#核心技术难点与解决方案)
- [项目亮点与创新点](#项目亮点与创新点)
- [量化成果](#量化成果)
- [扩展开发指南](#扩展开发指南)
- [项目职责与贡献要点](#项目职责与贡献要点)

---

## 项目概述

本项目是一个用 **C++17** 从零构建的 HTTP 反向代理网关，专为微服务架构设计。系统采用 **Reactor (epoll) + Thread Pool** 的经典架构，在单线程 Reactor 中处理所有网络 I/O，将计算密集型工作（请求解析、过滤器链、路由转发）卸载到线程池并行执行。

**核心应用场景：** 作为微服务集群的统一入口，接管客户端请求的路由分发、协议转换、限流保护、流量监控等横切关注点，解耦客户端与后端服务的直接依赖。

### 设计目标

- **高性能**：采用边缘触发 Epoll + 非阻塞 I/O，Release 模式启用 LTO + O3 + march=native 编译优化
- **高可用**：内置断路器、健康检查、自动故障转移等生产级容错机制
- **可扩展**：插件化过滤器链设计，支持请求/响应双向拦截
- **零依赖**：纯 C++17 标准库 + POSIX 系统调用，无任何第三方库

---

## 功能特性

### 核心功能矩阵

| 模块 | 功能 | 实现方式 |
|------|------|---------|
| **路由转发** | 最长前缀匹配 HTTP 反向代理 | `Router::match()`，支持多后端自动分发 |
| **负载均衡** | Round-Robin / Random / Least-Connections | `LoadBalancer::select()`，支持连接级别加权 |
| **断路器** | Closed → Open → HalfOpen 三态自动切换 | 失败计数阈值 + 超时冷却 + 试探恢复 |
| **健康检查** | 周期性 TCP Connect 存活探测 | 需连续 3 次失败才标记不健康，防抖动 |
| **过滤器链** | 请求/响应双阶段插件管道 | `on_request` 正向执行，`on_response` 逆序执行 |
| **速率限制** | 基于 IP 的令牌桶算法 | 可配置稳态速率和突发容量，自动清理过期桶 |
| **连接池** | 后端 TCP 连接复用 | per-host 空闲队列 + `is_alive()` 健康检查 + 定时淘汰 |
| **Keep-Alive** | HTTP/1.1 持久连接支持 | 优雅关闭（SHUT_WR）+ 空闲超时自动清理 |
| **异步日志** | 双缓冲异步写盘 | 独立后台线程，支持日志轮转（100MB 自动切分） |
| **实时监控** | /stats 端点输出 JSON 指标 | 原子计数器，线程安全的全局 + 分路由统计 |

### 内置过滤器

| 过滤器 | 阶段 | 功能 |
|--------|------|------|
| `RequestIdFilter` | on_request + on_response | 透传或生成 8 位 hex 追踪 ID，注入响应头 |
| `RateLimitFilter` | on_request | 基于客户端 IP 的令牌桶限流（默认 100rps / 200 burst） |
| `LoggingFilter` | on_request + on_response | 记录请求方法、路径、状态码、耗时 |

### 错误处理与状态码

| 场景 | HTTP 状态码 |
|------|------------|
| 请求体超过上限 | 413 Payload Too Large |
| 请求解析失败 | 400 Bad Request |
| 过滤器拒绝 | 400 Bad Request |
| 无匹配路由 | 404 Not Found |
| 后端连接/转发失败 | 502 Bad Gateway |
| 断路器熔断中 | 503 Service Unavailable |

---

## 技术栈选型

### 技术决策与理由

| 技术选择 | 替代方案 | 决策理由 |
|---------|---------|---------|
| **C++17** | C++14 / C++20 | `std::optional`, `std::string_view`, `if constexpr` 等特性刚好够用，编译器和生态支持最广泛 |
| **epoll (ET)** | poll / select / io_uring | 边缘触发 + 非阻塞 I/O 是 Linux 高并发标准化方案；io_uring 在 5.x 内核后方稳定，兼容性不足 |
| **epoll + Thread Pool** | 多 Reactor / Proactor | 权衡了实现复杂度与性能收益；多 Reactor 同步开销大，Proactor 需要 io_uring |
| **手写 JSON 解析器** | nlohmann/json / RapidJSON | 消除外部依赖，编译产物精简（<500KB），递归下降解析器代码量约 400 行 |
| **手写 HTTP/1.1 解析器** | llhttp / Boost.Beast | 完全控制状态机和增量解析逻辑，支持管线化请求残留处理 |
| **POSIX syscall** | libevent / libuv | 展示对操作系统底层 API 的掌控力，无运行时开销 |
| **O3 + LTO + march=native** | - | 编译器级别极致优化，利用 SIMD 和内联，使二进制体积和性能达到生产级别 |

### 编译优化配置

```cmake
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -flto=auto -ffat-lto-objects")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
```

LTO（链接时优化）跨编译单元内联和死代码消除，配合 `-march=native` 利用 CPU 原生指令集。

---

## 架构设计

### 整体架构图

```
                            ┌──────────────────────────────────────────┐
                            │              Event Loop Thread             │
                            │  ┌────────────────────────────────────┐   │
                            │  │         epoll_wait (ET mode)        │   │
                            │  │    ├── listen_fd (accept)           │   │
                            │  │    ├── client_fd (EPOLLIN read)     │   │
                            │  │    ├── client_fd (EPOLLOUT write)   │   │
                            │  │    ├── wakeup_fd (deferred tasks)   │   │
                            │  │    └── timer tick (idle callback)   │   │
                            │  └──────────────┬─────────────────────┘   │
                            └─────────────────┼────────────────────────┘
                                              │
          ┌───────────────────────────────────┼───────────────────────┐
          │                             submit(task)                  │
          │                        ┌───────┴────────┐                │
          │                        │   Thread Pool   │                │
          │                        │  (16 workers)   │                │
          │                        └───────┬────────┘                │
          │                                │                         │
          │     ┌──────────────────────────┼──────────────────────┐  │
          │     │              Gateway::process_request()          │  │
          │     │                                                  │  │
          │     │  1. parse_http_request()  ── 增量/一次性解析    │  │
          │     │  2. Filter Chain          ── on_request 正向    │  │
          │     │  3. Router::match()       ── 最长前缀匹配       │  │
          │     │  4. LoadBalancer::select() ── RoundRobin/Random │  │
          │     │  5. CircuitBreaker        ── 检查是否熔断       │  │
          │     │  6. Proxy::forward()      ── 后端 TCP 转发      │  │
          │     │  7. Stats::record()       ── 指标收集           │  │
          │     │  8. Filter Chain          ── on_response 逆序   │  │
          │     │  9. conn->send()          ── defer 回 Reactor   │  │
          │     └─────────────────────────────────────────────────┘  │
          └──────────────────────────────────────────────────────────┘
                                              │
                    ┌─────────────────────────┼─────────────────────────┐
                    │                         │                         │
              ┌─────▼─────┐           ┌──────▼──────┐          ┌──────▼──────┐
              │user_server│           │order_server │          │   (extend)  │
              │  :9001    │           │   :9002     │          │    :9003    │
              └───────────┘           └─────────────┘          └─────────────┘
                       BackendConnection Pool (per-host idle queue)
```

### 请求生命周期

```
Client                     Reactor Thread              Worker Thread              Backend
  │                             │                            │                        │
  │──── HTTP Request ──────────►│                            │                        │
  │                             │── epoll EPOLLIN ──►        │                        │
  │                             │── read_nonblock() ──►     │                        │
  │                             │── try_parse_message()     │                        │
  │                             │── submit(process) ────────►│                        │
  │                             │                            │── parse + filter      │
  │                             │                            │── route + LB select   │
  │                             │                            │── acquire connection  │── TCP Connect ──►│
  │                             │                            │── send_all() ──────────────────────────►│
  │                             │                            │◄── recv_all() ──────────────────────────│
  │                             │                            │── release connection  │                │
  │                             │◄── defer(send_response) ───│                        │                │
  │                             │── write_nonblock()         │                        │                │
  │◄─── HTTP Response ──────────┤                            │                        │                │
  │                             │ (keep-alive? → stay, else SHUT_WR → close)         │                │
```

---

## 项目结构

```
API_gateway/
├── CMakeLists.txt                    # 构建配置 (C++17/LTO/O3)
├── README.md
├── config.json                       # 运行时 JSON 配置
├── include/gateway/                  # 公共头文件（29个）
│   ├── core/                         # 核心模块
│   │   ├── config.h                  # 配置结构体 (Backend/Route/GatewayConfig)
│   │   ├── config_loader.h           # JSON 配置加载器（手写递归下降解析）
│   │   ├── gateway.h                 # 网关核心类（请求处理编排）
│   │   └── router.h                  # 路由器（最长前缀匹配）
│   ├── net/                          # 网络 I/O 层
│   │   ├── event_loop.h              # EventLoop（epoll 事件循环）
│   │   ├── tcp_server.h              # TCP 服务器（accept 循环）
│   │   ├── connection.h              # Connection 状态机（读写/解析/优雅关闭）
│   │   ├── buffer.h                  # Buffer（自动扩容环形缓冲区）
│   │   └── socket.h                  # Socket 工具函数
│   ├── http/                         # HTTP 协议层
│   │   ├── parser.h                  # HttpParser（增量状态机 + 一次性解析）
│   │   ├── request.h                 # HttpRequest 结构体
│   │   └── response.h                # HttpResponse 结构体
│   ├── proxy/                        # 代理与容错层
│   │   ├── proxy.h                   # Proxy（后端转发，直连/连接池）
│   │   ├── backend.h                 # BackendConnection（单后端 TCP 连接）
│   │   ├── backend_pool.h            # BackendPool（per-host 连接池）
│   │   ├── load_balancer.h           # LoadBalancer（RR/Random/LeastConn）
│   │   ├── circuit_breaker.h         # CircuitBreaker（Closed/Open/HalfOpen）
│   │   └── health_checker.h          # HealthChecker（TCP 连接存活探测）
│   ├── filter/                       # 可插拔过滤器
│   │   ├── filter.h                  # Filter 基类 + FilterContext 上下文
│   │   ├── request_id_filter.h       # 请求追踪 ID 过滤器
│   │   ├── rate_limit_filter.h       # 令牌桶限流过滤器
│   │   └── logging_filter.h          # 访问日志过滤器
│   ├── timer/
│   │   └── timer.h                   # Timer（最小堆定时器，支持周期/一次） 
│   ├── logger/
│   │   ├── logger.h                  # Logger（6 级日志 + 线程安全格式化）
│   │   └── async_logger.h            # AsyncLogger（双缓冲异步写盘）
│   ├── monitor/
│   │   └── stats.h                   # GatewayStats（原子计数器 + JSON 序列化）
│   └── utils/
│       ├── thread_pool.h             # ThreadPool（有界任务队列 + try_submit）
│       └── utils.h                   # 工具函数（安全类型转换等）
├── src/                              # 实现文件（29个，与 include 一一对应）
├── backend/                          # 后端模拟服务
│   ├── backend_worker.h              # 共享工作线程池 + keep-alive HTTP 处理器
│   ├── user_server.cpp               # 用户服务模拟（:9001）
│   └── order_server.cpp              # 订单服务模拟（:9002）
└── scripts/                          # 压测与运维脚本
    ├── bench_gateway.sh              # 一键阶梯压测
    ├── compare_bench.sh              # 多轮压测结果对比
    ├── run_test.sh                   # 快速冒烟测试
    └── wrk_mixed.lua                 # wrk Lua 脚本（50% GET + 50% POST）
```

---

## 快速开始

### 前置要求

- **编译器**：GCC 8+ / Clang 7+（需完全支持 C++17）
- **构建工具**：CMake 3.10+
- **操作系统**：Linux 4.x+（依赖 epoll / eventfd / sendfile 等内核 API）

### 构建编译

```bash
cd API_gateway
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

产物：
- `build/gateway` — 网关主程序
- `build/user_server` — 用户服务模拟器
- `build/order_server` — 订单服务模拟器

### 启动运行

```bash
# 1. 启动后端模拟服务
./build/user_server &    # 监听 9001
./build/order_server &   # 监听 9002

# 2. 启动网关
./build/gateway --log-level=warn
# 监听 8080，加载 config.json
```

### 配置说明

所有配置通过 `config.json` 管理，支持命令行覆盖：

```json
{
    "listen_port": 8080,
    "keep_alive_timeout_sec": 3600,
    "thread_count": 16,
    "backend_timeout_ms": 3000,
    "max_request_size": 65536,
    "max_epoll_events": 512,
    "idle_cleanup_interval_sec": 5,
    "pool_max_idle_per_host": 10,
    "pool_idle_timeout_sec": 60,
    "max_deferred_per_round": 256,
    "routes": [
        {
            "prefix": "/api/user",
            "backends": [{"host": "127.0.0.1", "port": 9001}]
        },
        {
            "prefix": "/api/order",
            "backends": [{"host": "127.0.0.1", "port": 9002}]
        }
    ]
}
```

| 参数 | 类型 | 说明 |
|------|------|------|
| `listen_port` | int | 网关监听端口 |
| `keep_alive_timeout_sec` | int | Keep-Alive 空闲超时 |
| `thread_count` | int | 线程池大小 |
| `backend_timeout_ms` | int | 后端连接/转发超时 |
| `max_request_size` | int | 请求体上限（字节，64KB） |
| `max_epoll_events` | int | 每轮 epoll_wait 最大事件数 |
| `pool_max_idle_per_host` | int | 每后端最大空闲连接数 |
| `max_deferred_per_round` | int | 每轮 Reactor 最大延迟任务数 |
| `routes` | array | 路由表：prefix → [backends] |

---

## 使用指南

### API 端点

| 端点 | 方法 | 说明 |
|------|------|------|
| `/api/user` | `*` | 代理到用户服务（user_server :9001） |
| `/api/order` | `*` | 代理到订单服务（order_server :9002） |
| `/health` | `*` | 网关存活检查，返回 `OK` |
| `/stats` | `*` | 实时监控指标（JSON），返回全局 + 分路由统计 |

### 命令行参数

```
./gateway [--config=<path>] [--log-level=<level>] [--port=<N>] [--threads=<N>] [--timeout=<ms>]
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--config=` | `config.json` | 配置文件路径 |
| `--log-level=` | `info` | 日志级别（trace/debug/info/warn/error/fatal） |
| `--port=` | （config） | 覆盖监听端口 |
| `--threads=` | （config） | 覆盖线程池大小 |
| `--timeout=` | （config） | 覆盖后端转发超时 |

### 功能验证

```bash
# 基础请求
curl http://localhost:8080/api/user
curl http://localhost:8080/api/order -d '{"item":"test"}'

# 自定义 X-Request-Id
curl -H "X-Request-Id: my-trace-001" http://localhost:8080/api/user

# Keep-Alive 复用连接
curl -v http://localhost:8080/api/user http://localhost:8080/api/order

# 查看实时监控指标
curl http://localhost:8080/stats | python3 -m json.tool

# 错误场景测试
curl http://localhost:8080/api/unknown    # → 404
```

### 压测工具

```bash
# 一键阶梯压测（50→100→200→400→800 连接，30s/档）
./scripts/bench_gateway.sh

# 自定义参数
THREADS=16 DURATION=60s CONNECTIONS_LIST="100 200 500 1000" ./scripts/bench_gateway.sh

# 多轮压测结果对比
./scripts/compare_bench.sh
```

---

## 核心模块详解

### 1. EventLoop — epoll Reactor

```
EPOLLET 边缘触发 + eventfd 跨线程唤醒 + 定时器集成
```

- 所有 fd 注册为 `EPOLLET`（边缘触发），`on_read`/`on_write` 必须循环至 EAGAIN
- 通过 `eventfd` 实现跨线程唤醒：`defer()` 向 eventfd 写入 1 字节，Reactor 在下次 epoll_wait 时处理积压任务
- `max_deferred_per_round`（默认 256）限制每轮 defer 处理数，防 I/O 饿死
- 动态 epoll_wait 超时：根据 Timer 最近到期时间计算，默认 1000ms

### 2. Connection — 连接状态机

```
Reading → Processing → Writing → (keep-alive ? Reading : SHUT_WR → Closing → Closed)
```

- 增量 HTTP 解析：Buffer 边读边解析，一次 `epoll_wait` 可能处理多个完整请求（管线化支持）
- 使用 `shared_from_this()` 确保跨线程安全：Worker 线程持有 `shared_ptr<Connection>`，防止 fd 复用导致 use-after-free
- 优雅关闭：非 keep-alive 时发送 `shutdown(fd, SHUT_WR)`，等待客户端 EOF 后 `close()`
- 请求体上限检查：`max_request_size`（64KB），超限立即关闭（413 Payload Too Large）

### 3. HttpParser — HTTP/1.1 增量解析器

```
RequestLine → Headers → (Content-Length Body | Chunked Body) → Complete
```

- 支持 `Content-Length` 和 `chunked` 两种传输模式
- `partial_` 缓冲区暂存未完整行/块，`reset()` 时保留管线化残余数据
- 同时提供 `parse_http_request()` 一次性解析（供 Worker 线程使用）

### 4. ThreadPool — 有界任务队列

- `submit()` 阻塞提交（队列满时等待 `cv_not_full_`）
- `try_submit()` 非阻塞提交（队列满时立即返回 false）
- 使用 `std::packaged_task` + `std::future` 支持异步获取返回值
- 线程安全：`std::mutex` + `std::condition_variable` 保护

### 5. BackendPool — 后端连接池

```
acquire() → idle queue pop (check is_alive) → create new if empty
release() → is_alive ? push idle : evict
cleanup_idle() → 定时淘汰超时空闲连接
```

- `is_alive()` 双重检查：`getsockopt(SO_ERROR)` 检测连接错误 + `recv(MSG_PEEK|MSG_DONTWAIT)` 检测对端关闭
- `active_count` 追踪正在使用的连接数，防泄漏

### 6. CircuitBreaker — 断路器

```
Closed → (failures ≥ threshold) → Open → (timeout expired) → HalfOpen → (probes succeed) → Closed
                                                                  └→ (any failure) → Open
```

- 闭路状态：放行所有请求，连续失败 5 次后熔断
- 开路状态：拒绝所有请求，`reset_timeout_ms` 后自动转为半开
- 半开状态：允许 3 个试探请求通过，全部成功则恢复闭路；任一失败则回退开路

### 7. LoadBalancer — 负载均衡器

- **RoundRobin**：原子计数器取模，无锁实现
- **Random**：线程本地 Mersenne Twister 随机数生成器
- **LeastConnections**：连接计数器 + 互斥锁保护，选择最少活跃连接的后端

### 8. HealthChecker — 健康检查器

- TCP Connect 探测，需 **连续 3 次失败**才标记不健康（防网络抖动误判）
- 状态变更回调：通知负载均衡器从候选池中移除/恢复后端

### 9. 过滤器链

```
RequestIdFilter → RateLimitFilter → LoggingFilter
      ↓                                  ↓
  on_request (正向)               on_response (逆序)
```

- `Filter::on_request()` 返回 `false` 中断处理链，直接返回 400
- `Filter::on_response()` 可修改响应头（例：注入 `X-Request-Id`）
- `FilterContext` 携带 req、resp、request_id、start_time 等上下文

### 10. Stats — 实时监控指标

```json
{
    "total_requests": 500000,
    "total_success": 499980,
    "total_errors": 20,
    "avg_latency_ms": 2.3,
    "max_latency_ms": 250,
    "in_flight": 5,
    "upstream_errors": 15,
    "circuit_breaker_trips": 1,
    "rate_limited": 4,
    "routes": {
        "/api/user": { "total": 300000, "success": 299995, "errors": 5, ... },
        "/api/order": { "total": 200000, "success": 199985, "errors": 15, ... }
    }
}
```

所有计数器使用 `std::atomic`，最大延迟通过 CAS 循环更新，线程安全。

---

## 核心技术难点与解决方案

### 难点 1：Reactor 单线程的 I/O 公平性问题

**问题：** 单 Reactor 线程同时处理 accept、read、write、timer tick 和 defer 任务，高并发下可能因为 defer 任务过多导致新连接 accept 被饿死。

**解决：** 引入 `max_deferred_per_round`（默认 256）限制每轮 epoll 循环最多处理的 defer 任务数，超出部分顺延至下一轮，保证 I/O 事件处理公平性。

### 难点 2：跨线程 use-after-free 风险

**问题：** Worker 线程完成代理转发后，通过 `EventLoop::defer()` 将响应写回客户端。如果 `Connection` 在此期间被过期清理或 fd 被复用，`send()` 可能写错 fd 或操作野指针。

**解决：** 所有跨线程回调使用 `shared_from_this()` 捕获 `shared_ptr<Connection>`，确保对象在整个生命周期内有效。`Finish` 回调负责从 `connections_` map 中移除并递减引用。

### 难点 3：HTTP 管线化请求的边界处理

**问题：** HTTP/1.1 在 Keep-Alive 模式下，客户端可能在同一个 TCP 连接上连续发送多个请求。如果解析器不处理管线化，残余数据会被丢弃，导致后续请求丢失。

**解决：** 增量解析器 `HttpParser` 在 `reset()` 时保留 `partial_` 缓冲区中的未完成行/块。`pending_size()` 暴露残余字节数，连接层据此判断是否有待处理的管线化数据，选择性触发 `try_parse_message()`。

### 难点 4：连接池的僵尸连接检测

**问题：** 后端服务可能意外关闭连接（重启、超时），但连接池无法感知，导致失败。

**解决：** `is_alive()` 双重探测机制：
1. `getsockopt(SO_ERROR)` — 检测内核协议栈层面的错误
2. `recv(MSG_PEEK | MSG_DONTWAIT)` — 无破坏性地检测对端是否已关闭（返回 0 表示 EOF）

### 难点 5：性能瓶颈定位与调优

**问题：** 初始版本中，网关代理 QPS 仅为后端直连的 32.6%（2,861 vs 8,779），平均延迟 16.8ms。

**诊断过程：**
- 逐模块压测定位：后端直连 → 经网关代理 → 分析各层级耗时
- 内部 `/stats` 指标 vs 客户端 `wrk` 指标交叉验证
- 排除法定位：TCP_NODELAY（-80%）、pool_max_idle_per_host 过大（-60%）、max_epoll_events 过大（-50%）

**关键发现：**
- `listen(128)` 是瓶颈 — 高并发时 accept 队列溢出
- `listen(SOMAXCONN)` 后峰值 QPS 提升至 6,309（+73%）
- 单 Reactor 架构下 TCP_NODELAY 是性能毒药：禁用 Nagle 后小包事件激增，Reactor 过载

---

## 项目亮点与创新点

### 架构亮点

1. **零外部依赖的完整链路**：从 HTTP 解析、JSON 配置加载、异步日志到 epoll 事件循环，全部使用 C++17 标准库 + POSIX syscall 手写实现，无任何第三方库，编译产物 < 500KB。

2. **生产级容错机制**：断路器（三态自动切换）+ 健康检查（3 次失败才标记）+ 连接池自动回收 + 优雅关闭，覆盖微服务通信中的常见故障模式。

3. **请求级追踪**：`X-Request-Id` 全链路透传，贯穿过滤器链、路由、代理、日志，便于分布式追踪和问题定位。

### 工程亮点

4. **系统化的性能调优方法论**：通过逐模块隔离压测 + 控制变量法，在 8 个可调参数中精准识别唯一有效优化点（listen backlog），同时排除 3 个反模式（TCP_NODELAY / 大连接池 / 大 epoll 数组）。

5. **线程安全的日志系统**：双缓冲异步日志 + 100MB 自动切分，日志写入不阻塞 I/O 线程。

6. **自包含的基准测试套件**：`bench_gateway.sh` 一键阶梯压测 + `compare_bench.sh` 多轮结果对比，自动化性能回归检测。

### 代码质量

7. **RAII 资源管理**：所有 socket fd 由 RAII 封装管理，异常安全，无资源泄漏。

8. **类型安全的封装**：`std::optional<HttpRequest>` 消除空指针检查，`std::string_view` 零拷贝解析，`std::atomic` 无锁计数。

---

## 量化成果

### 性能指标

| 指标 | 数值 |
|------|------|
| 纯后端直连 QPS（user_server） | 8,779 Req/s |
| 网关代理 QPS（50 并发） | 5,259 Req/s（优化后峰值） |
| 网关代理 QPS（200 并发） | 6,309 Req/s（优化后峰值） |
| 网关内部处理延迟 | ≤1.0ms |
| 支持最大并发连接 | 10,000（可配置） |
| 请求体上限 | 64KB（可配置的 DoS 防护） |

### 系统规格

| 维度 | 详情 |
|------|------|
| 代码规模 | 29 头文件 + 29 源文件，约 8,000+ 行 C++ |
| 编译产物 | gateway ≈ 480KB（Release + LTO + strip） |
| 外部依赖 | **0** |
| 支持协议 | HTTP/1.1（含 Keep-Alive、管线化、chunked 编码） |
| 线程模型 | 1 Reactor + N Worker（默认 16） |

---

## 扩展开发指南

### 添加新路由

在 `config.json` 的 `routes` 数组中添加：

```json
{"prefix": "/api/product", "backends": [{"host": "127.0.0.1", "port": 9003}]}
```

### 自定义过滤器

```cpp
// include/gateway/filter/auth_filter.h
class AuthFilter : public Filter {
public:
    bool on_request(FilterContext& ctx) override {
        // 检查 Authorization 头
        if (!ctx.req.has_header("authorization")) {
            ctx.resp = make_401();
            return false;  // 中断处理链
        }
        return true;
    }
    void on_response(FilterContext&) override {}
};

// main.cpp 中注册
gw.add_filter(std::make_unique<AuthFilter>());
```

### 添加新负载均衡算法

继承 `LoadBalancer` 基类，实现 `select(const std::vector<Backend>&)` 方法即可。

### 添加后端服务

参考 `backend/user_server.cpp` 模板，使用 `backend_worker.h` 中的 `BackendWorkerPool` + `handle_client_keepalive()`。

---

## 项目职责与贡献要点

> 以下内容面向简历与面试展示，可根据实际角色删减。

### 技术职责

- 独立设计并实现网关核心架构：Reactor 事件循环 + 线程池 + 过滤器链 + 路由代理全链路
- 手写实现 HTTP/1.1 增量解析器（Content-Length + chunked 传输编码、管线化支持）
- 实现生产级容错组件：断路器（三态切换）、健康检查（3 次连续失败触发）、后端连接池（复用 + 僵尸检测）
- 实现异步双缓冲日志系统（100MB 自动切分）+ 实时监控指标（`/stats` 端点，原子计数器无锁统计）
- 排查并解决 5+ 线上潜在故障：use-after-free（`shared_from_this` 改造）、死锁（回调锁释放）、单 Reactor I/O 饿死（defer 上限）等

### 贡献亮点

- 通过系统化压测和瓶颈分析，将网关代理吞吐从 **2,861 QPS 提升至 6,309 QPS（+121%）**，延迟从 16.8ms 降至稳定水平
- 定位并修复 `listen(128)` backlog 瓶颈，验证单 Reactor 架构下性能天花板，为后续架构升级提供数据支撑
- 建立完整的性能基准测试流程：自动阶梯压测 + 多轮对比 + 回归检测

### 可量化的项目成果

| 维度 | 优化前 | 优化后 | 提升 |
|------|--------|--------|------|
| 网关代理峰值 QPS | 3,655 | 6,309 | **+73%** |
| 内部处理延迟 | 3.76ms | ≤1.0ms | **-73%** |
| 编译产物体积 | ~500KB | ~480KB | 精简 |
| 外部依赖 | 0 | 0 | — |
| 容错组件覆盖 | 基础路由代理 | +断路器 + 健康检查 + 连接池 + 限流 | 全面 |

### 关键技术栈

`C++17` `epoll` `Reactor` `Thread Pool` `HTTP/1.1` `Circuit Breaker` `Load Balancer` `Connection Pool` `Token Bucket` `Non-blocking I/O` `JSON Parser` `Async Logger` `Performance Tuning`

---

## License

MIT
