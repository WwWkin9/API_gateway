#include "gateway/net/buffer.h"

#include <cerrno>
#include <cstring>
#include <sys/uio.h>
#include <unistd.h>

namespace buffer {
    // 对短模式使用 memmem（GNU 扩展），比 KMP 省去预处理开销
    const char* fast_search(const char* haystack, size_t haystack_size,
                            const char* needle, size_t needle_size) {
        if (needle_size == 0) return haystack;
        if (haystack_size < needle_size) return nullptr;
        return static_cast<const char*>(::memmem(haystack, haystack_size,
                                                 needle, needle_size));
    }
}

Buffer::Buffer() 
    : buf_(kInitialSize + kPrependableSize)
    , read_idx_(kPrependableSize)
    , write_idx_(kPrependableSize) {}


// ============== 从 fd 读取 ==============

ssize_t Buffer::read_from_fd(int fd) {
    // 方案：readv 一次系统调用同时写栈缓冲区和 vector 尾部
    //   - 如果 vector 尾部空间足够 → 直接写尾部
    //   - 如果尾部空间不够 → 先整理碎片再写
    // 栈缓冲区兜底，避免 readv 失败时丢失数据

    char stack_buf[65536];  // 64KB 缓冲区
    const size_t writable = writable_size();

    // 配置 readv 参数
    struct iovec iov[2];
    iov[0].iov_base = writable_data();
    iov[0].iov_len = writable;
    iov[1].iov_base = stack_buf;
    iov[1].iov_len = sizeof(stack_buf);

    // 计算 readv 参数数量
    //   - 如果 vector 尾部空间足够 → 直接写尾部
    //   - 如果尾部空间不够 → 先整理碎片再写
    int iovcnt = (writable >= sizeof(stack_buf)) ? 1 : 2;

    ssize_t ret = readv(fd, iov, iovcnt);
    if (ret < 0) {
        return -1;
    }
    if (ret == 0) {
        return 0;
    }

    if (static_cast<size_t>(ret) <= writable) {
        write_idx_ += ret;
    } else {
        write_idx_ = buf_.size();
        append(stack_buf, static_cast<size_t>(ret - writable));
    }

    return ret;
}

// ============== 写入 fd ==============
ssize_t Buffer::write_to_fd(int fd) {
    if (readable_size() == 0) {
        return 0;
    }
    ssize_t ret = write(fd, readable_data(), readable_size());
    if (ret > 0) {
        consume(static_cast<size_t>(ret));
    }
    return ret;
}

// ============== 消费数据 ==============
void Buffer::consume(size_t n) {
    if (n > readable_size()) {
        n = readable_size();
    }
    read_idx_ += n;

    if (read_idx_ == write_idx_) {
        read_idx_ = kPrependableSize;
        write_idx_ = kPrependableSize;
    }
}

void Buffer::consume_all() {
    read_idx_ = kPrependableSize;
    write_idx_ = kPrependableSize;
}

// ============== 查找 ==============
const char* Buffer::find(const char* needle, size_t needle_size) const {
    if (needle_size == 0 || readable_size() < needle_size) {
        return nullptr;
    }
    return buffer::fast_search(readable_data(), readable_size(), needle, needle_size);
}

const char* Buffer::find(char c) const {
    return static_cast<const char*>(::memchr(readable_data(), c, readable_size()));
}

const char* Buffer::find_crlf() const {
    return find("\r\n", 2);
}

const char* Buffer::find_double_crlf() const {
    return find("\r\n\r\n", 4);
}

// ============== 追加数据 ==============
void Buffer::append(const char* data, size_t n) {
    if (n == 0) {
        return;
    }
    ensure_writable(n);
    std::memcpy(writable_data(), data, n);
    write_idx_ += n;
}

void Buffer::append(const Buffer& other) {
    append(other.readable_data(), other.readable_size());
}

// ============== 头部插入（prepend） ==============
void Buffer::prepend(const char* data, size_t n) {
    if (n > prependable_size()) {
        size_t needed = n + readable_size();
        std::vector<char> new_buf(needed + kPrependableSize);
        new_buf.resize(needed + kPrependableSize);

        size_t new_read = kPrependableSize + n;
        std::memcpy(new_buf.data() + new_read, readable_data(), readable_size());

        buf_.swap(new_buf);
        read_idx_ = new_read;
        write_idx_ = read_idx_ + readable_size();
    }

    read_idx_ -= n;
    std::memcpy(buf_.data() + read_idx_, data, n);
}

// ============== 空间管理 ==============
void Buffer::ensure_writable(size_t n) {
    if (writable_size() >= n) return;

    size_t needed = n - writable_size();
    if (prependable_size() + writable_size() >= n + kPrependableSize) {
        shrink();
    } else {
        buf_.resize(buf_.size() + needed);
    }
}

void Buffer::shrink() {
    if (read_idx_ == kPrependableSize) return;

    size_t resize = readable_size();
    if (resize > 0){
        std::memmove(buf_.data() + kPrependableSize, readable_data(), resize);
    }
    read_idx_ = kPrependableSize;
    write_idx_ = kPrependableSize + resize;
}

void Buffer::swap(Buffer& other) {
    buf_.swap(other.buf_);
    std::swap(read_idx_, other.read_idx_);
    std::swap(write_idx_, other.write_idx_);
}
