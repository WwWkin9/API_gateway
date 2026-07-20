#pragma once

// Socket 工具：封装 socket 创建、绑定、监听等底层操作
//
// 用法：
//   int listen_fd = create_listen_socket(8080);
//   set_nonblocking(client_fd);

// 设置文件描述符为非阻塞模式
// 成功返回 0，失败返回 -1
int set_nonblocking(int fd);

// 创建监听 socket（已设置 SO_REUSEADDR + 非阻塞 + listen(128)）
// 成功返回 fd，失败返回 -1
int create_listen_socket(int port);
