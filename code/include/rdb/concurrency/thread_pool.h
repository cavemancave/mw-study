// 固定线程数 + 有界任务队列的线程池。
// 无界队列的线程池会把背压问题藏起来，直到 OOM 才暴露，所以这里必须有界并显式拒绝。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "rdb/concurrency/bounded_queue.h"

namespace rdb {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t threads, std::size_t queue_capacity = 1024)
        : queue_(queue_capacity, OverflowPolicy::Block) {
        const std::size_t n = threads == 0 ? 1 : threads;
        workers_.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            workers_.emplace_back([this] { run(); });
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    ~ThreadPool() { stop(true); }

    // 队列满时阻塞调用方，形成真背压。
    bool post(std::function<void()> task) {
        return queue_.push(std::move(task)) == PushStatus::Ok;
    }

    // 队列满时立即失败，调用方必须处理"任务被拒绝"这条路径。
    bool try_post(std::function<void()> task) {
        return queue_.try_push(std::move(task)) == PushStatus::Ok;
    }

    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::optional<std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>> {
        using Result = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
        using Optional = std::optional<std::future<Result>>;

        auto callable = [fn = std::forward<F>(f),
                         bound = std::make_tuple(std::forward<Args>(args)...)]() mutable -> Result {
            return std::apply(std::move(fn), std::move(bound));
        };
        auto task = std::make_shared<std::packaged_task<Result()>>(std::move(callable));
        std::future<Result> future = task->get_future();
        if (!post([task] { (*task)(); })) return Optional{};
        return Optional{std::move(future)};
    }

    // drain=true：跑完已入队任务再退出；false：丢弃排队任务，尽快停。
    void stop(bool drain) {
        bool expected = false;
        if (!stopping_.compare_exchange_strong(expected, true)) return;
        if (!drain) queue_.clear();
        queue_.close();
        for (std::thread& t : workers_) {
            if (t.joinable()) t.join();
        }
        workers_.clear();
    }

    std::size_t thread_count() const { return workers_.size(); }
    std::size_t pending() const { return queue_.size(); }
    std::uint64_t completed() const { return completed_.load(std::memory_order_relaxed); }
    std::size_t queue_high_water() const { return queue_.high_water(); }

private:
    void run() {
        for (;;) {
            std::optional<std::function<void()>> task = queue_.pop();
            if (!task) return;  // 队列已关闭且取空
            (*task)();
            completed_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    BoundedQueue<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> completed_{0};
};

}  // namespace rdb
