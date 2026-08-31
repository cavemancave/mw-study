#include "rdb/msg/buffer_pool.h"

#include <cstring>
#include <memory>
#include <vector>

#include "rdb/testing/check.h"

using namespace rdb;

RDB_TEST(buffer_pool_acquire_and_return) {
    auto pool = BufferPool::create(1024, 4);
    RDB_CHECK_EQ(pool->available(), std::size_t{4});
    {
        BufferPtr buffer = pool->acquire();
        RDB_CHECK(buffer != nullptr);
        RDB_CHECK_EQ(buffer->capacity(), std::size_t{1024});
        RDB_CHECK_EQ(buffer->size(), std::size_t{0});
        RDB_CHECK(buffer->resize(512));
        RDB_CHECK(!buffer->resize(2048));  // 不允许超出容量
        RDB_CHECK_EQ(pool->available(), std::size_t{3});
        RDB_CHECK_EQ(pool->in_use(), std::size_t{1});
    }
    RDB_CHECK_EQ(pool->available(), std::size_t{4});
}

// 池满不再分配，而是返回空句柄：让"内存不够"成为可观测的背压信号。
RDB_TEST(buffer_pool_exhaustion_is_visible) {
    auto pool = BufferPool::create(64, 2);
    BufferPtr a = pool->acquire();
    BufferPtr b = pool->acquire();
    BufferPtr c = pool->acquire();
    RDB_CHECK(a != nullptr);
    RDB_CHECK(b != nullptr);
    RDB_CHECK(c == nullptr);
    RDB_CHECK_EQ(pool->exhausted_count(), std::uint64_t{1});

    a.reset();
    BufferPtr d = pool->acquire();
    RDB_CHECK(d != nullptr);
}

// 一次填充、多个订阅者共享同一份内存：这就是进程内的零拷贝扇出。
RDB_TEST(buffer_pool_fanout_shares_one_copy) {
    auto pool = BufferPool::create(256, 1);
    BufferPtr producer = pool->acquire();
    RDB_CHECK(producer != nullptr);
    producer->resize(4);
    std::memcpy(producer->data(), "abcd", 4);

    std::vector<BufferPtr> subscribers(3, producer);
    const std::byte* base = producer->data();
    for (const BufferPtr& s : subscribers) {
        RDB_CHECK(s->data() == base);
    }

    producer.reset();
    RDB_CHECK_EQ(pool->available(), std::size_t{0});  // 仍有订阅者持有
    subscribers.clear();
    RDB_CHECK_EQ(pool->available(), std::size_t{1});  // 引用计数归零才还池
}

RDB_TEST(buffer_pool_handle_outlives_pool) {
    BufferPtr buffer;
    {
        auto pool = BufferPool::create(32, 1);
        buffer = pool->acquire();
        RDB_CHECK(buffer != nullptr);
    }
    RDB_CHECK(buffer->resize(16));  // 池已析构，句柄仍然安全
    buffer.reset();
}
