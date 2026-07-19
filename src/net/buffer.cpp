#include "gateway/net/buffer.h"

#include <cerrno>
#include <sys/uio.h>
#include <unistd.h>

namespace buffer {
    std::vector<int> getNext(const char* str, size_t str_size){
        std::vector<int> next;
        next.push_back(0);
        size_t prefix_len = 0;
        size_t i = 1;
        while (i < str_size) {
            if (str[i] == str[prefix_len]) {
                prefix_len++;
                next.push_back(prefix_len);
            } else {
                if (prefix_len == 0) {
                    next.push_back(0);
                    i++;
                } else {
                    prefix_len = next[prefix_len - 1];
                }
            }
        }
        return next;
    }
    const char* kmpSearch(const char* str, size_t str_size, const char* needle, size_t needle_size){
        std::vector<int> next = getNext(needle, needle_size);
        size_t i = 0;
        size_t j = 0;
        while (i < str_size && j < needle_size) {
            if (str[i] == needle[j]) {
                i++;
                j++;
            } else {
                if (j == 0) {
                    i++;
                } else {
                    j = next[j - 1];
                }
            }
        }
        if (j == needle_size) {
            return str + i - j;
        }
        return nullptr;
    }
};

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
    
    const char* data = readable_data();
    return buffer::kmpSearch(
        data, 
        readable_size(), 
        needle, 
        needle_size
    ); 
}

const char* Buffer::find(char c) const {
    const char* date = readable_data();
    const char* end = date + readable_size();
    
    for (const char* p = date; p != end; ++p) {
        if (*p == c) {
            return p;
        }
    }
    return nullptr;
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
