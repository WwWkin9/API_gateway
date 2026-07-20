#include "gateway/net/connection.h"
#include "gateway/http/parser.h"   // parse_content_length, HttpParser
#include "gateway/logger/logger.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

// ============== 构造 / 析构 ==============

Connection::Connection(EventLoop* event_loop, int fd, int max_request_size)
    : event_loop_(event_loop)
    , fd_(fd)
    , state_(State::Reading)
    , last_active_time_(std::time(nullptr))
    , max_request_size_(max_request_size)
{
    parser_.set_max_size(max_request_size_);
}

Connection::~Connection() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void Connection::set_message_callback(MessageCallback cb) {
    message_callback_ = std::move(cb);
}

void Connection::set_close_callback(CloseCallback cb) {
    close_callback_ = std::move(cb);
}

// ============== 读事件处理 ==============

void Connection::on_read() {
    // Closing 状态：已发送 FIN，只需等待客户端 EOF
    if (state_ == State::Closing) {
        char drain[64];
        while (::recv(fd_, drain, sizeof(drain), 0) > 0) {}
        close_internal();
        return;
    }
    if (state_ != State::Reading) { LOG_DEBUG("on_read fd=%d skip state=%d", fd_, (int)state_); return; }

    last_active_time_ = std::time(nullptr);
    LOG_DEBUG("on_read fd=%d start", fd_);

    // 循环读取直到 EAGAIN，一次性取走内核缓冲区的所有数据
    while (true) {
        ssize_t n = read_buf_.read_from_fd(fd_);
        if (n == 0) {
            close_internal();      // 对端关闭
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_internal();
            return;
        }

        // 防 DoS：缓冲区太大直接断开
        if (read_buf_.readable_size() > static_cast<size_t>(max_request_size_)) {
            close_internal();
            return;
        }
    }

    LOG_DEBUG("on_read fd=%d try_parse_message", fd_);
    try_parse_message();
}

// ============== 消息解析（增量状态机） ==============

void Connection::try_parse_message() {
    // 当 read_buf_ 有数据或 parser 的 partial_ 中有管道请求残留时继续尝试解析
    while (!read_buf_.empty() || parser_.pending_size() > 0) {
        size_t consumed = parser_.parse(
            read_buf_.readable_data(),
            read_buf_.readable_size());

        read_buf_.consume(consumed);

        if (parser_.has_error()) {
            LOG_WARN("try_parse_message fd=%d parser error, raw_size=%zu", fd_, parser_.raw().size());
            close_internal();
            return;
        }

        if (parser_.complete()) {
            std::string raw = parser_.raw();
            parser_.reset();

            state_ = State::Processing;
            LOG_DEBUG("parse complete fd=%d raw_len=%zu calling message_cb", fd_, raw.size());
            if (message_callback_) message_callback_(fd_, std::move(raw));
            return;  // 等待上层处理完成后再解析下一条
        }

        // consumed == 0 → 数据不够完整一行，退出等更多数据
        if (consumed == 0) {
            LOG_DEBUG("try_parse_message fd=%d consumed=0 waiting more data", fd_);
            return;
        }
    }
}

// ============== 写事件处理 ==============

void Connection::on_write() {
    if (state_ != State::Writing) return;

    last_active_time_ = std::time(nullptr);
    LOG_DEBUG("on_write fd=%d start buf_size=%zu", fd_, write_buf_.readable_size());

    // 循环写直到写缓冲清空或内核缓冲区满
    while (!write_buf_.empty()) {
        ssize_t n = write_buf_.write_to_fd(fd_);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;  // 等下次 EPOLLOUT
            close_internal();
            return;
        }
        if (n == 0) return;  // 暂时写不进去，等下次 EPOLLOUT
    }

    finish_response();
}

void Connection::finish_response() {
    if (keep_alive_) {
        state_ = State::Reading;
        event_loop_->mod_fd(fd_, EPOLLIN);
        parser_.reset();
        // 处理已读入 partial_ 中的管道请求数据
        if (parser_.pending_size() > 0) {
            try_parse_message();
        }
    } else {
        // 优雅关闭：shutdown(SHUT_WR) 发送 FIN，避免 close() 触发 RST
        // 然后等待客户端读取完数据后关闭（客户端 EOF 时 on_read 会调用 close_internal）
        ::shutdown(fd_, SHUT_WR);
        state_ = State::Closing;
        event_loop_->mod_fd(fd_, EPOLLIN);
    }
}

// ============== 发送响应 ==============

// 可从任意线程调用：实际发送转交给事件循环线程执行，
// 捕获 shared_from_this() 保证任务执行时连接对象仍然存活
void Connection::send(const std::string& data, bool keep_alive) {
    LOG_DEBUG("send fd=%d len=%zu ka=%d deferring", fd_, data.size(), (int)keep_alive);
    auto self = shared_from_this();
    event_loop_->defer([self, data, keep_alive]() {
        self->send_internal(data, keep_alive);
    });
}

void Connection::send_internal(const std::string& data, bool keep_alive) {
    if (state_ != State::Processing && state_ != State::Reading) {
        LOG_DEBUG("send_internal fd=%d skip state=%d", fd_, (int)state_);
        return;
    }
    LOG_DEBUG("send_internal fd=%d len=%zu ka=%d", fd_, data.size(), (int)keep_alive);
    keep_alive_ = keep_alive;
    write_buf_.append(data);

    state_ = State::Writing;
    event_loop_->mod_fd(fd_, EPOLLIN | EPOLLOUT);

    on_write();
}

// ============== 关闭 ==============

void Connection::force_close() {
    if (state_ == State::Closed) return;
    auto self = shared_from_this();
    event_loop_->defer([self]() {
        self->close_internal();
    });
}

void Connection::close_internal() {
    if (state_ == State::Closed) return;
    int fd = fd_;
    fd_ = -1;  // 防止析构函数重复 close
    LOG_DEBUG("close_internal fd=%d", fd);
    state_ = State::Closed;

    // 缓存指针，防止 close_callback_ 销毁 this 后访问成员变量
    EventLoop* loop = event_loop_;
    auto cb = std::move(close_callback_);

    loop->del_fd(fd);
    if (cb) cb(fd);
    loop->remove_callback(fd);

    ::close(fd);
    // 注意：此函数结束后 Connection 可能已被销毁
}
