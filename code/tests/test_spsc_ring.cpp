#include "rdb/concurrency/spsc_ring.h"

#include <atomic>
#include <thread>

#include "rdb/testing/check.h"

using namespace rdb;

RDB_TEST(spsc_ring_capacity_rounds_up_to_power_of_two) {
    SpscRing<int> ring(5);
    RDB_CHECK_EQ(ring.capacity(), std::size_t{8});
    SpscRing<int> exact(16);
    RDB_CHECK_EQ(exact.capacity(), std::size_t{16});
}

RDB_TEST(spsc_ring_push_pop_fifo_and_full) {
    SpscRing<int> ring(4);
    for (int i = 0; i < 4; ++i) RDB_CHECK(ring.push(i));
    RDB_CHECK(!ring.push(99));  // 满了必须失败，而不是覆盖
    RDB_CHECK_EQ(ring.size(), std::size_t{4});

    int out = -1;
    for (int i = 0; i < 4; ++i) {
        RDB_CHECK(ring.pop(out));
        RDB_CHECK_EQ(out, i);
    }
    RDB_CHECK(!ring.pop(out));
    RDB_CHECK(ring.empty());
}

// 反复填满取空，检查下标回绕后仍然正确。
RDB_TEST(spsc_ring_wraps_around) {
    SpscRing<int> ring(2);
    int out = 0;
    for (int round = 0; round < 1000; ++round) {
        RDB_CHECK(ring.push(round));
        RDB_CHECK(ring.pop(out));
        RDB_CHECK_EQ(out, round);
    }
}

RDB_TEST(spsc_ring_single_producer_single_consumer_preserves_order) {
    constexpr int kCount = 200000;
    SpscRing<int> ring(1024);
    std::atomic<bool> ordered{true};
    std::atomic<int> received{0};

    std::thread consumer([&] {
        int expected = 0;
        int value = 0;
        while (expected < kCount) {
            if (ring.pop(value)) {
                if (value != expected) ordered.store(false);
                ++expected;
                received.store(expected, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    for (int i = 0; i < kCount; ++i) {
        while (!ring.push(i)) std::this_thread::yield();
    }
    consumer.join();

    RDB_CHECK(ordered.load());
    RDB_CHECK_EQ(received.load(), kCount);
}
