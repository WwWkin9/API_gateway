cmake_minimum_required(VERSION 3.10)
project(cpp_gateway)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(gateway
    src/main.cpp
    src/config.cpp
    src/http_request.cpp
    src/http_response.cpp
    src/gateway.cpp
    src/utils.cpp
    src/thread_pool.cpp
    src/filter_logging.cpp
    src/filter_request_id.cpp
)

add_executable(user_server backend/user_server.cpp)
add_executable(order_server backend/order_server.cpp)