cmake_minimum_required(VERSION 3.10)
project(cpp_gateway)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(gateway
    src/main.cpp

    # core
    src/core/config.cpp
    src/core/config_loader.cpp
    src/core/gateway.cpp
    src/core/router.cpp

    # net
    src/net/event_loop.cpp
    src/net/tcp_server.cpp
    src/net/connection.cpp
    src/net/buffer.cpp
    src/net/socket.cpp

    # http
    src/http/parser.cpp
    src/http/request.cpp
    src/http/response.cpp

    # proxy
    src/proxy/proxy.cpp
    src/proxy/backend_pool.cpp
    src/proxy/load_balancer.cpp
    src/proxy/circuit_breaker.cpp
    src/proxy/backend.cpp
    src/proxy/health_checker.cpp

    # filter
    src/filter/logging_filter.cpp
    src/filter/request_id_filter.cpp
    src/filter/rate_limit_filter.cpp    

    # logger
    src/logger/logger.cpp
    src/logger/async_logger.cpp

    # timer
    src/timer/timer.cpp

    # monitor
    src/monitor/stats.cpp

    # utils
    src/utils/utils.cpp
    src/utils/thread_pool.cpp
)

add_executable(user_server backend/user_server.cpp)
add_executable(order_server backend/order_server.cpp)
