#pragma once

// Socket 工具：封装 socket 创建、绑定、监听等底层操作
//
// 用法：
//   int listen_fd = create_listen_socket(8080);
//   set_nonblocking(client_fd);

// 设置文件描述符为非阻塞模式
// 成功返回 0，失败返回 -1
int set_nonblocking(int fd);

// 禁用 Nagle 算法，减少小包延迟
// 成功返回 0，失败返回 -1
int set_tcp_nodelay(int fd);

// 创建监听 socket（已设置 SO_REUSEADDR + TCP_NODELAY + 非阻塞 + listen(SOMAXCONN)）
// 成功返回 fd，失败返回 -1
int create_listen_socket(int port);
