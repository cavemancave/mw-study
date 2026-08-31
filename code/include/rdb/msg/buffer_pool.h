// 定长缓冲区池：图像/点云这类大消息不能每帧 new。
// 池满时返回空句柄而不是继续分配 —— 让"内存不够"变成可观测的背压信号，而不是 OOM。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace rdb {

class BufferPool;

class Buffer {
public:
    std::byte* data() { return storage_.data(); }
    const std::byte* data() const { return storage_.data(); }
    std::size_t size() const { return size_; }
    std::size_t capacity() const { return storage_.size(); }

    bool resize(std::size_t n) {
        if (n > storage_.size()) return false;
        size_ = n;
        return true;
    }

private:
    friend class BufferPool;
    explicit Buffer(std::size_t bytes) : storage_(bytes) {}

    std::vector<std::byte> storage_;
    std::size_t size_ = 0;
};

// 共享句柄：一次发布、多个订阅者持有同一份数据，引用计数归零才还池。
using BufferPtr = std::shared_ptr<Buffer>;

class BufferPool : public std::enable_shared_from_this<BufferPool> {
public:
    static std::shared_ptr<BufferPool> create(std::size_t buffer_bytes, std::size_t count) {
        auto pool = std::shared_ptr<BufferPool>(new BufferPool(buffer_bytes, count));
        pool->preallocate();
        return pool;
    }

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // 返回 nullptr 表示池已耗尽：调用方必须决定丢帧、降采样还是阻塞等待。
    BufferPtr acquire() {
        std::unique_ptr<Buffer> raw;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (free_.empty()) {
                ++exhausted_;
                return nullptr;
            }
            raw = std::move(free_.back());
            free_.pop_back();
        }
        raw->size_ = 0;
        // 句柄可能比池活得久，必须用 weak_ptr 而不是裸 this，否则还池时读到已释放内存。
        std::weak_ptr<BufferPool> weak = weak_from_this();
        return BufferPtr(raw.release(), [weak](Buffer* p) {
            if (std::shared_ptr<BufferPool> pool = weak.lock()) {
                pool->release(p);
            } else {
                delete p;
            }
        });
    }

    std::size_t buffer_bytes() const { return buffer_bytes_; }
    std::size_t total() const { return total_; }

    std::size_t available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return free_.size();
    }

    std::size_t in_use() const { return total_ - available(); }

    // 耗尽次数是容量是否合理的直接证据，应该导出成监控指标。
    std::uint64_t exhausted_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return exhausted_;
    }

private:
    BufferPool(std::size_t buffer_bytes, std::size_t count)
        : buffer_bytes_(buffer_bytes == 0 ? 1 : buffer_bytes), total_(count == 0 ? 1 : count) {}

    void preallocate() {
        std::lock_guard<std::mutex> lock(mutex_);
        free_.reserve(total_);
        for (std::size_t i = 0; i < total_; ++i) {
            free_.emplace_back(new Buffer(buffer_bytes_));
        }
    }

    void release(Buffer* p) {
        std::unique_ptr<Buffer> owned(p);
        owned->size_ = 0;
        std::lock_guard<std::mutex> lock(mutex_);
        free_.push_back(std::move(owned));
    }

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Buffer>> free_;
    std::size_t buffer_bytes_;
    std::size_t total_;
    std::uint64_t exhausted_ = 0;
};

}  // namespace rdb
