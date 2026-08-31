// 有界队列：中间件里所有"上游快于下游"的地方都靠它兜底。
// 关键在于容量有限时的三种策略，以及 close() 后的优雅收敛。
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace rdb {

enum class OverflowPolicy {
    Block,       // 阻塞生产者：真背压，代价是慢消费者会拖住上游
    DropNewest,  // 丢新：保住已排队的旧数据，适合"必须按序处理"的流
    DropOldest,  // 丢旧：保最新状态，适合传感器/状态类话题
};

enum class PushStatus {
    Ok,
    Full,          // 非阻塞或超时路径下队列已满，调用方需自行决定重试或降级
    DroppedNewest,
    DroppedOldest,
    Closed,
};

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity, OverflowPolicy policy = OverflowPolicy::Block)
        : capacity_(capacity == 0 ? 1 : capacity), policy_(policy) {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    PushStatus push(T value) {
        return push_until(std::move(value), std::chrono::steady_clock::time_point::max());
    }

    PushStatus push_for(T value, std::chrono::nanoseconds timeout) {
        return push_until(std::move(value), std::chrono::steady_clock::now() + timeout);
    }

    PushStatus try_push(T value) {
        return push_until(std::move(value), std::chrono::steady_clock::time_point::min());
    }

    PushStatus push_until(T value, std::chrono::steady_clock::time_point deadline) {
        // 丢弃的元素搬到锁外再析构：元素析构函数是用户代码，可能又去拿别的锁。
        std::optional<T> evicted;
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_) return PushStatus::Closed;

        if (items_.size() >= capacity_) {
            switch (policy_) {
                case OverflowPolicy::DropNewest:
                    ++dropped_;
                    return PushStatus::DroppedNewest;
                case OverflowPolicy::DropOldest:
                    evicted.emplace(std::move(items_.front()));
                    items_.pop_front();
                    ++dropped_;
                    items_.push_back(std::move(value));
                    not_empty_.notify_one();
                    return PushStatus::DroppedOldest;
                case OverflowPolicy::Block:
                    break;
            }
            if (!wait_for_space(lock, deadline)) return PushStatus::Full;
            if (closed_) return PushStatus::Closed;
        }

        items_.push_back(std::move(value));
        if (items_.size() > high_water_) high_water_ = items_.size();
        // 在持锁时 notify：略损失一点并发度，换来"等待方醒来后销毁队列"不会踩到野指针。
        not_empty_.notify_one();
        return PushStatus::Ok;
    }

    std::optional<T> pop() {
        return pop_until(std::chrono::steady_clock::time_point::max());
    }

    std::optional<T> pop_for(std::chrono::nanoseconds timeout) {
        return pop_until(std::chrono::steady_clock::now() + timeout);
    }

    std::optional<T> try_pop() {
        return pop_until(std::chrono::steady_clock::time_point::min());
    }

    // 关闭后仍可取走已入队的数据，取空才返回 nullopt —— 这是"优雅停止"的关键。
    std::optional<T> pop_until(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (items_.empty() && !closed_) {
            if (deadline == std::chrono::steady_clock::time_point::max()) {
                not_empty_.wait(lock, [this] { return closed_ || !items_.empty(); });
            } else if (deadline == std::chrono::steady_clock::time_point::min()) {
                return std::nullopt;
            } else if (!not_empty_.wait_until(
                           lock, deadline, [this] { return closed_ || !items_.empty(); })) {
                return std::nullopt;
            }
        }
        if (items_.empty()) return std::nullopt;

        T value = std::move(items_.front());
        items_.pop_front();
        not_full_.notify_one();
        return std::optional<T>(std::move(value));
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    void clear() {
        std::deque<T> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            discarded.swap(items_);
            not_full_.notify_all();
        }
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    bool empty() const { return size() == 0; }
    std::size_t capacity() const { return capacity_; }
    OverflowPolicy policy() const { return policy_; }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    std::uint64_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

    // 高水位是容量是否合理的直接证据：长期贴着 capacity 说明下游产能不足。
    std::size_t high_water() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return high_water_;
    }

private:
    bool wait_for_space(std::unique_lock<std::mutex>& lock,
                        std::chrono::steady_clock::time_point deadline) {
        auto ready = [this] { return closed_ || items_.size() < capacity_; };
        if (deadline == std::chrono::steady_clock::time_point::max()) {
            not_full_.wait(lock, ready);
            return true;
        }
        if (deadline == std::chrono::steady_clock::time_point::min()) return false;
        return not_full_.wait_until(lock, deadline, ready);
    }

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> items_;
    std::size_t capacity_;
    OverflowPolicy policy_;
    bool closed_ = false;
    std::uint64_t dropped_ = 0;
    std::size_t high_water_ = 0;
};

}  // namespace rdb
