// 队列吞吐对比：有锁有界队列 vs 无锁 SPSC 环。
// 结论不是"无锁一定快"，而是要看争用程度、批量大小和唤醒次数。
//
// 用法示例：
//   ./bench_queue_throughput --count 2000000 --capacity 1024 --producers 4
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <thread>
#include <vector>

#include "args.h"
#include "rdb/common/time.h"
#include "rdb/concurrency/bounded_queue.h"
#include "rdb/concurrency/spsc_ring.h"

namespace {

struct Result {
    double seconds = 0.0;
    double msgs_per_sec = 0.0;
    std::uint64_t consumed = 0;
};

void report(const char* name, const Result& r) {
    std::printf("%-28s consumed=%-10llu %8.3f s  %10.2f M msg/s\n", name,
                static_cast<unsigned long long>(r.consumed), r.seconds,
                r.msgs_per_sec / 1e6);
}

Result run_bounded_queue(long long count, long long capacity, long long producers) {
    rdb::BoundedQueue<std::uint64_t> queue(static_cast<std::size_t>(capacity),
                                           rdb::OverflowPolicy::Block);
    std::atomic<std::uint64_t> consumed{0};
    const std::uint64_t start = rdb::now_ns();

    std::thread consumer([&] {
        for (;;) {
            if (!queue.pop().has_value()) return;
            consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::vector<std::thread> writers;
    const long long per_producer = count / producers;
    for (long long p = 0; p < producers; ++p) {
        writers.emplace_back([&queue, per_producer] {
            for (long long i = 0; i < per_producer; ++i) {
                queue.push(static_cast<std::uint64_t>(i));
            }
        });
    }
    for (std::thread& t : writers) t.join();
    queue.close();
    consumer.join();

    Result r;
    r.seconds = static_cast<double>(rdb::now_ns() - start) / 1e9;
    r.consumed = consumed.load();
    r.msgs_per_sec = static_cast<double>(r.consumed) / (r.seconds > 0 ? r.seconds : 1.0);
    return r;
}

Result run_spsc_ring(long long count, long long capacity) {
    rdb::SpscRing<std::uint64_t> ring(static_cast<std::size_t>(capacity));
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<bool> done{false};
    const std::uint64_t start = rdb::now_ns();

    std::thread consumer([&] {
        std::uint64_t value = 0;
        std::uint64_t seen = 0;
        while (seen < static_cast<std::uint64_t>(count)) {
            if (ring.pop(value)) {
                ++seen;
            } else if (done.load(std::memory_order_acquire)) {
                if (!ring.pop(value)) break;
                ++seen;
            } else {
                std::this_thread::yield();
            }
        }
        consumed.store(seen);
    });

    for (long long i = 0; i < count; ++i) {
        while (!ring.push(static_cast<std::uint64_t>(i))) std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
    consumer.join();

    Result r;
    r.seconds = static_cast<double>(rdb::now_ns() - start) / 1e9;
    r.consumed = consumed.load();
    r.msgs_per_sec = static_cast<double>(r.consumed) / (r.seconds > 0 ? r.seconds : 1.0);
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace rdb::bench;
    const long long count = arg_int(argc, argv, "--count", 2000000);
    const long long capacity = arg_int(argc, argv, "--capacity", 1024);
    const long long producers = arg_int(argc, argv, "--producers", 4);

    std::printf("config: count=%lld capacity=%lld producers=%lld\n", count, capacity, producers);
    std::printf("note: numbers are only meaningful with -O2 and no sanitizer\n\n");

    report("BoundedQueue 1P1C", run_bounded_queue(count, capacity, 1));
    report("BoundedQueue NP1C", run_bounded_queue(count, capacity, producers));
    report("SpscRing 1P1C", run_spsc_ring(count, capacity));
    return 0;
}
