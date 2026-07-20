#include "gateway/net/connection.h"
#include "gateway/http/parser.h"   // parse_content_length

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
{}

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
        // 对端关闭
        close_internal();
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;  // 正常，稍后再读
        close_internal();
        return;
    }

    // 防 DoS：检查缓冲区是否超过 max_request_size
    if (read_buf_.readable_size() > static_cast<size_t>(max_request_size_)) {
        close_internal();
        return;
    }

    // 尝试解析完整消息
    try_parse_message();
}

// ============== 消息解析 ==============

bool Connection::try_parse_message() {
    while (true) {
        if (!header_parsed_) {
            const char* header_end = read_buf_.find_double_crlf();
            if (!header_end) return false;

            const char* data = read_buf_.readable_data();
            size_t header_len = header_end - data;

            pending_content_length_ = parse_content_length(data, header_len);

            // 拒绝超大 Content-Length
            if (pending_content_length_ > max_request_size_) {
                close_internal();
                return false;
            }

            header_parsed_ = true;

            // 无 body 或 Content-Length == 0，消息已完整
            if (pending_content_length_ <= 0) {
                size_t total_len = header_len + 4;
                std::string raw(data, total_len);
                read_buf_.consume(total_len);

                state_ = State::Processing;
                if (message_callback_) message_callback_(fd_, std::move(raw));
                return true;
            }
        }

        // 步骤 2：等待 body 数据完整
        if (pending_content_length_ > 0){
            const char* head_end_pos = read_buf_.find_double_crlf();
            size_t header_total_len = (head_end_pos - read_buf_.readable_data()) + 4;
            
            if (read_buf_.readable_size() >= header_total_len + static_cast<size_t>(pending_content_length_)){
                // body 已完整接收，提取整个 raw 数据，包含后面的 \r\n\r\n
                size_t total_len = header_total_len + static_cast<size_t>(pending_content_length_);
                std::string raw(read_buf_.readable_data(), total_len);
                read_buf_.consume(total_len);

                // 重置解析状态，准备下一个请求
                header_parsed_ = false;
                pending_content_length_ = -1;

                state_ = State::Processing;
                if (message_callback_) message_callback_(fd_, std::move(raw));
                return true;
            }
        }
        return false; // body 数据不完整，继续等待
    }
}

// ============== 写事件处理 ==============

void Connection::on_write() {
    if (state_ != State::Writing) return;

    last_active_time_ = std::time(nullptr);
    // 没有待发送数据，切换回读模式（或关闭）
    if (write_buf_.empty()) { finish_response(); return; }
    
    ssize_t n = write_buf_.write_to_fd(fd_);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;  // 正常，稍后再写
        close_internal();
        return;
    }

    if (write_buf_.empty()) { finish_response(); }
}

void Connection::finish_response() {
    if (keep_alive_) {
        state_ = State::Reading;
        event_loop_->mod_fd(fd_, EPOLLIN | EPOLLET);

        header_parsed_ = false;
        pending_content_length_ = -1;
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
    // 注册 EPOLLOUT 事件
    event_loop_->mod_fd(fd_, EPOLLOUT | EPOLLET);

    // 触发写事件处理
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