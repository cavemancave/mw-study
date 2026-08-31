#include "rdb/concurrency/thread_pool.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "rdb/testing/check.h"

using namespace rdb;

RDB_TEST(thread_pool_submit_returns_future) {
    ThreadPool pool(2, 16);
    auto future = pool.submit([](int a, int b) { return a + b; }, 20, 22);
    RDB_CHECK(future.has_value());
    RDB_CHECK_EQ(future->get(), 42);
}

RDB_TEST(thread_pool_runs_all_posted_tasks) {
    constexpr int kTasks = 5000;
    ThreadPool pool(4, 128);
    std::atomic<int> counter{0};
    for (int i = 0; i < kTasks; ++i) {
        RDB_CHECK(pool.post([&counter] { counter.fetch_add(1, std::memory_order_relaxed); }));
    }
    pool.stop(true);
    RDB_CHECK_EQ(counter.load(), kTasks);
    RDB_CHECK_EQ(pool.completed(), static_cast<std::uint64_t>(kTasks));
}

// 有界队列 + try_post 让"任务被拒绝"变成可处理的返回值，而不是无声堆积。
RDB_TEST(thread_pool_try_post_rejects_when_saturated) {
    ThreadPool pool(1, 2);
    std::atomic<bool> release{false};
    RDB_CHECK(pool.post([&release] {
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }));

    int accepted = 0;
    int rejected = 0;
    for (int i = 0; i < 20; ++i) {
        if (pool.try_post([] {})) {
            ++accepted;
        } else {
            ++rejected;
        }
    }
    release.store(true, std::memory_order_release);
    pool.stop(true);

    RDB_CHECK(rejected > 0);
    RDB_CHECK(accepted <= 3);
}

RDB_TEST(thread_pool_stop_without_drain_discards_pending) {
    ThreadPool pool(1, 256);
    std::atomic<bool> release{false};
    std::atomic<int> done{0};
    pool.post([&release] {
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    for (int i = 0; i < 100; ++i) {
        pool.try_post([&done] { done.fetch_add(1, std::memory_order_relaxed); });
    }

    // 唯一的 worker 还卡在第一个任务里，stop(false) 先清空队列再关闭，
    // 因此后面 100 个任务一个都不会执行。
    std::thread releaser([&release] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        release.store(true, std::memory_order_release);
    });
    pool.stop(false);
    releaser.join();
    RDB_CHECK_EQ(done.load(), 0);
}

RDB_TEST(thread_pool_destructor_drains) {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(3, 64);
        for (int i = 0; i < 500; ++i) {
            pool.post([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        }
    }
    RDB_CHECK_EQ(counter.load(), 500);
}
