// 单生产者单消费者无锁环形队列。
// 用它对比 BoundedQueue 可以直观看到：锁不是"慢"，而是唤醒和上下文切换贵。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace rdb {

namespace detail {
inline std::size_t round_up_pow2(std::size_t v) {
    if (v < 2) return 2;
    --v;
    for (std::size_t shift = 1; shift < sizeof(std::size_t) * 8; shift <<= 1) {
        v |= v >> shift;
    }
    return v + 1;
}
}  // namespace detail

// 要求 T 可默认构造且可移动赋值。容量向上取整到 2 的幂，用掩码代替取模。
template <typename T>
class SpscRing {
public:
    explicit SpscRing(std::size_t capacity)
        : mask_(detail::round_up_pow2(capacity) - 1), slots_(mask_ + 1) {}

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    std::size_t capacity() const { return mask_ + 1; }

    // 只允许生产者线程调用。
    bool push(T value) {
        const std::size_t write = write_.load(std::memory_order_relaxed);
        if (write - read_cache_ == capacity()) {
            read_cache_ = read_.load(std::memory_order_acquire);
            if (write - read_cache_ == capacity()) return false;
        }
        slots_[write & mask_] = std::move(value);
        // release 保证消费者看到序号推进时，槽位内容已经写完。
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    // 只允许消费者线程调用。
    bool pop(T& out) {
        const std::size_t read = read_.load(std::memory_order_relaxed);
        if (read == write_cache_) {
            write_cache_ = write_.load(std::memory_order_acquire);
            if (read == write_cache_) return false;
        }
        out = std::move(slots_[read & mask_]);
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

    std::size_t size() const {
        const std::size_t write = write_.load(std::memory_order_acquire);
        const std::size_t read = read_.load(std::memory_order_acquire);
        return write - read;
    }

    bool empty() const { return size() == 0; }

private:
    static constexpr std::size_t kCacheLine = 64;

    std::size_t mask_;
    std::vector<T> slots_;

    // 生产者和消费者各占独立缓存行，否则两个原子量互相拉扯（false sharing）。
    alignas(kCacheLine) std::atomic<std::size_t> write_{0};
    std::size_t read_cache_ = 0;   // 仅生产者访问
    alignas(kCacheLine) std::atomic<std::size_t> read_{0};
    std::size_t write_cache_ = 0;  // 仅消费者访问
};

}  // namespace rdb
