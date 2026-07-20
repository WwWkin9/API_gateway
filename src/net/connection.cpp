#include "gateway/net/connection.h"
#include "gateway/http/parser.h"   // parse_content_length, HttpParser

#include <sys/epoll.h>
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
    if (state_ != State::Reading) return;

    last_active_time_ = std::time(nullptr);

    ssize_t n = read_buf_.read_from_fd(fd_);
    if (n == 0) {
        close_internal();      // 对端关闭
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        close_internal();
        return;
    }

    // 防 DoS：缓冲区太大直接断开
    if (read_buf_.readable_size() > static_cast<size_t>(max_request_size_)) {
        close_internal();
        return;
    }

    try_parse_message();
}

// ============== 消息解析（增量状态机） ==============

void Connection::try_parse_message() {
    while (!read_buf_.empty()) {
        size_t consumed = parser_.parse(
            read_buf_.readable_data(),
            read_buf_.readable_size());

        read_buf_.consume(consumed);

        if (parser_.has_error()) {
            close_internal();
            return;
        }

        if (parser_.complete()) {
            std::string raw = parser_.raw();
            parser_.reset();

            state_ = State::Processing;
            if (message_callback_) message_callback_(fd_, std::move(raw));
            return;  // 等待上层处理完成后再解析下一条
        }

        // consumed == 0 → 数据不够完整一行，退出等更多数据
        if (consumed == 0) return;
    }
}

// ============== 写事件处理 ==============

void Connection::on_write() {
    if (state_ != State::Writing) return;

    last_active_time_ = std::time(nullptr);

    if (write_buf_.empty()) {
        finish_response();
        return;
    }

    ssize_t n = write_buf_.write_to_fd(fd_);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        close_internal();
        return;
    }

    if (write_buf_.empty()) finish_response();
}

void Connection::finish_response() {
    if (keep_alive_) {
        state_ = State::Reading;
        event_loop_->mod_fd(fd_, EPOLLIN | EPOLLET);
        parser_.reset();
    } else {
        close_internal();
    }
}

// ============== 发送响应 ==============

void Connection::send(const std::string& data, bool keep_alive) {
    if (state_ != State::Processing && state_ != State::Reading) return;
    keep_alive_ = keep_alive;
    write_buf_.append(data);

    state_ = State::Writing;
    event_loop_->mod_fd(fd_, EPOLLOUT | EPOLLET);

    on_write();
}

// ============== 关闭 ==============

void Connection::force_close() {
    if (state_ == State::Closed) return;
    close_internal();
}

void Connection::close_internal() {
    if (state_ == State::Closed) return;
    state_ = State::Closed;

    event_loop_->del_fd(fd_);
    event_loop_->remove_callback(fd_);

    ::close(fd_);

    if (close_callback_) close_callback_(fd_);

    fd_ = -1;
}
