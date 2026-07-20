#include "gateway/net/tcp_server.h"
#include "gateway/net/socket.h"
#include "gateway/logger/logger.h"

#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

TCPServer::TCPServer(EventLoop* event_loop, int port)
    : event_loop_(event_loop), port_(port) {}

TCPServer::~TCPServer() {
    if (listen_fd_ >= 0) {
        event_loop_->del_fd(listen_fd_);
        event_loop_->remove_callback(listen_fd_);
        ::close(listen_fd_);
    }
}

void TCPServer::set_new_connection_callback(NewConnectionCallback cb) {
    new_connection_cb_ = std::move(cb);
}

void TCPServer::start() {
    listen_fd_ = create_listen_socket(port_);
    if (listen_fd_ < 0) {
        LOG_ERROR("TCPServer::start: create_listen_socket failed on port %d: %s", port_, strerror(errno));
        return;
    }

    event_loop_->add_fd(listen_fd_, EPOLLIN);
    event_loop_->set_callback(listen_fd_, [this](int fd, uint32_t /*events*/){
        (void)fd; // 忽略 fd，因为 accept 会返回新连接的 fd
        on_accept();
    });
}

void TCPServer::on_accept() {
    while (true) {
        sockaddr_in peer_addr;
        socklen_t addr_len = sizeof(peer_addr);

        int cfd = ::accept(listen_fd_, (struct sockaddr*)&peer_addr, &addr_len);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 非阻塞模式下没有新连接
            if (errno == EMFILE || errno == ENFILE) {
                LOG_WARN("TCPServer::on_accept: accept failed: %s", strerror(errno));
                continue; // 文件描述符耗尽，但 epoll 已水平触发，等下次 accept
            }
            break;
        }

        // 设置非阻塞模式
        if (set_nonblocking(cfd) < 0) {
            LOG_ERROR("TCPServer::on_accept: set_nonblocking failed: %s", strerror(errno));
            ::close(cfd);
            continue;
        }

        // 获取对端 IP
        char ip_buf[INET_ADDRSTRLEN];
        const char* ip = inet_ntop(AF_INET, &peer_addr.sin_addr, ip_buf, sizeof(ip_buf));
        int port = ntohs(peer_addr.sin_port);

        std::string peer_ip = ip ? ip : "UNKNOWN";
        int peer_port = port;
        
        // 通知上层有新连接发生
        if (new_connection_cb_) {
            new_connection_cb_(cfd, peer_ip, peer_port);
        } else {
            // 没有回调，关闭连接
            ::close(cfd);
        }
    }
}