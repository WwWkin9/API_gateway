#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>


// 高效的读写缓冲区，基于 vector + 双索引（类似 muduo 设计）
//
// 内部布局：
//   [ prependable | readable | writable ]
//   0           read_idx_  write_idx_   buf_.size()
//
// - prependable:  预留空间，可在数据前插入少量字节（如加 HTTP 头）
// - readable:     待消费/待发送的数据
// - writable:     可写入新数据的空间
class Buffer {
public:
    static constexpr size_t kInitialSize = 4096;    // 初始缓冲区大小    
    static constexpr size_t kPrependableSize = 128; // 预留空间大小（用于在数据前插入少量字节）

    Buffer();

    // ---- 从 fd 读写 ----

    ssize_t read_from_fd(int fd);
    ssize_t write_to_fd(int fd);

    // ---- 读端操作（消费数据） ----

    // 获取可读数据的起始位置
    const char* readable_data() const { return buf_.data() + read_idx_; }

    // 获取可读数据的大小
    size_t readable_size() const { return write_idx_ - read_idx_; }

    // 获取可读数据的字符串表示
    std::string read_as_string() const { return std::string(readable_data(), readable_size()); }

    // 消费可读n个字节的数据
    void consume(size_t n);

    // 消费所有可读数据
    void consume_all();

    // 获取可读数据的第 offset 个字节
    char peek(size_t offset) const { return *(readable_data() + offset); }

    // 查找可读数据中是否存在 needle 字符串
    const char* find(const char* needle, size_t needle_size) const;
    const char* find(const std::string& needle) const {
        return find(needle.data(), needle.size());
    }

    // 查找可读数据中是否存在单字符 c
    const char* find(char c) const;

    // 查找可读数据中是否存在 CRLF 字符串(\r\n)
    const char* find_crlf() const;

    // 查找可读数据中是否存在 Double CRLF 字符串(\r\n\r\n)
    const char* find_double_crlf() const;

    // ---- 写端操作（追加数据） ----

    // 追加原始数据
    void append(const char* data, size_t n);
    void append(const std::string& data) { append(data.c_str(), data.size()); }

    // 追加其他缓冲区的数据
    void append(const Buffer& other);
    
    // 在数据前追加原始数据(典型用于在数据前插入 HTTP 头)
    void prepend(const char* data, size_t n);
    void prepend(const std::string& data) { prepend(data.c_str(), data.size()); }
    
    // 获取可写入数据的起始位置
    char* writable_data() { return buf_.data() + write_idx_; }
        
    // 获取可写入数据的大小
    size_t writable_size() const { return buf_.size() - write_idx_; }

    // 标记已写入 n 个字节
    void has_writeen(size_t n) { write_idx_ += n; }

    // ---- 内部空间管理 ----

    // 确保缓冲区至少有 n 个字节的可写入空间
    void ensure_writable(size_t n);

    // 整理碎片：把可读数据移到 buffer 头部，回收 prependable 空间
    void shrink();

    // 检查缓冲区是否为空
    bool empty() const { return readable_size() == 0; }

    // 清空缓冲区
    void clear() { read_idx_ = kPrependableSize; write_idx_ = kPrependableSize; }
    
    // 交换两个缓冲区的内容
    void swap(Buffer& other);
        
private:
    std::vector<char> buf_;
    size_t read_idx_;           // 读取索引，指向待消费数据的起始位置
    size_t write_idx_;          // 写入索引，指向可写入新数据的位置

    // 获取预留空间大小
    size_t prependable_size() const { return read_idx_; }
};