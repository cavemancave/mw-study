#include "rdb/concurrency/bounded_queue.h"

#include <atomic>
#include <thread>
#include <vector>

#include "rdb/testing/check.h"

using namespace rdb;

RDB_TEST(bounded_queue_basic_fifo) {
    BoundedQueue<int> q(4);
    RDB_CHECK_EQ(q.capacity(), std::size_t{4});
    RDB_CHECK(q.empty());
    for (int i = 0; i < 4; ++i) RDB_CHECK(q.push(i) == PushStatus::Ok);
    RDB_CHECK_EQ(q.size(), std::size_t{4});
    for (int i = 0; i < 4; ++i) {
        auto v = q.pop();
        RDB_CHECK(v.has_value());
        RDB_CHECK_EQ(*v, i);
    }
    RDB_CHECK(q.empty());
    RDB_CHECK_EQ(q.high_water(), std::size_t{4});
}

RDB_TEST(bounded_queue_try_push_full_does_not_block) {
    BoundedQueue<int> q(2, OverflowPolicy::Block);
    RDB_CHECK(q.try_push(1) == PushStatus::Ok);
    RDB_CHECK(q.try_push(2) == PushStatus::Ok);
    RDB_CHECK(q.try_push(3) == PushStatus::Full);
    RDB_CHECK_EQ(q.size(), std::size_t{2});
}

RDB_TEST(bounded_queue_drop_newest_keeps_oldest) {
    BoundedQueue<int> q(2, OverflowPolicy::DropNewest);
    RDB_CHECK(q.push(1) == PushStatus::Ok);
    RDB_CHECK(q.push(2) == PushStatus::Ok);
    RDB_CHECK(q.push(3) == PushStatus::DroppedNewest);
    RDB_CHECK_EQ(q.dropped(), std::uint64_t{1});
    RDB_CHECK_EQ(*q.pop(), 1);
    RDB_CHECK_EQ(*q.pop(), 2);
}

RDB_TEST(bounded_queue_drop_oldest_keeps_newest) {
    BoundedQueue<int> q(2, OverflowPolicy::DropOldest);
    RDB_CHECK(q.push(1) == PushStatus::Ok);
    RDB_CHECK(q.push(2) == PushStatus::Ok);
    RDB_CHECK(q.push(3) == PushStatus::DroppedOldest);
    RDB_CHECK_EQ(q.dropped(), std::uint64_t{1});
    RDB_CHECK_EQ(*q.pop(), 2);
    RDB_CHECK_EQ(*q.pop(), 3);
}

RDB_TEST(bounded_queue_pop_timeout_returns_nullopt) {
    BoundedQueue<int> q(2);
    const auto start = std::chrono::steady_clock::now();
    auto v = q.pop_for(std::chrono::milliseconds(20));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    RDB_CHECK(!v.has_value());
    RDB_CHECK(elapsed >= std::chrono::milliseconds(15));
}

// close() 之后仍能取走已入队数据，取空才结束：这是优雅停止的必要语义。
RDB_TEST(bounded_queue_close_drains_then_ends) {
    BoundedQueue<int> q(4);
    q.push(1);
    q.push(2);
    q.close();
    RDB_CHECK(q.push(3) == PushStatus::Closed);
    RDB_CHECK_EQ(*q.pop(), 1);
    RDB_CHECK_EQ(*q.pop(), 2);
    RDB_CHECK(!q.pop().has_value());
}

RDB_TEST(bounded_queue_close_wakes_blocked_consumer) {
    BoundedQueue<int> q(2);
    std::atomic<bool> finished{false};
    std::thread consumer([&] {
        auto v = q.pop();
        finished.store(!v.has_value());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    q.close();
    consumer.join();
    RDB_CHECK(finished.load());
}

RDB_TEST(bounded_queue_close_wakes_blocked_producer) {
    BoundedQueue<int> q(1, OverflowPolicy::Block);
    RDB_CHECK(q.push(1) == PushStatus::Ok);
    std::atomic<int> status{-1};
    std::thread producer([&] { status.store(static_cast<int>(q.push(2))); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    q.close();
    producer.join();
    RDB_CHECK(static_cast<PushStatus>(status.load()) == PushStatus::Closed);
}

RDB_TEST(bounded_queue_multi_producer_multi_consumer_conserves_items) {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 3;
    constexpr int kPerProducer = 2000;

    BoundedQueue<int> q(64, OverflowPolicy::Block);
    std::atomic<long long> consumed_sum{0};
    std::atomic<int> consumed_count{0};

    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            for (;;) {
                auto v = q.pop();
                if (!v) return;
                consumed_sum.fetch_add(*v, std::memory_order_relaxed);
                consumed_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) q.push(p * kPerProducer + i);
        });
    }
    for (auto& t : producers) t.join();
    q.close();
    for (auto& t : consumers) t.join();

    constexpr int kTotal = kProducers * kPerProducer;
    long long expected = 0;
    for (int i = 0; i < kTotal; ++i) expected += i;
    RDB_CHECK_EQ(consumed_count.load(), kTotal);
    RDB_CHECK_EQ(consumed_sum.load(), expected);
    RDB_CHECK(q.high_water() <= q.capacity());
}
