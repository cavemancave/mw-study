// 端到端发布订阅延迟压测。
// 回答的问题：在给定频率、负载大小、订阅者数量和 QoS 下，
// p50/p99/p999 分别是多少，丢了多少，缓冲池是否耗尽。
//
// 用法示例：
//   ./bench_bus_latency --rate 1000 --count 200000 --subscribers 3 --payload 4096
//   ./bench_bus_latency --rate 200 --count 20000 --best-effort --slow-ms 5
//
// 注意：Reliable + --slow-ms 会让发布者被慢订阅者拖住（这正是可靠 QoS 的代价），
// 观察这一点请用很小的 --count。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "args.h"
#include "rdb/bus/topic_bus.h"
#include "rdb/common/time.h"
#include "rdb/metrics/histogram.h"
#include "rdb/msg/buffer_pool.h"

namespace {

struct Sample {
    std::uint64_t seq = 0;
    std::uint64_t publish_ns = 0;
    rdb::BufferPtr payload;
};

struct SubscriberState {
    rdb::Histogram latency;
    std::uint64_t received = 0;
    std::uint64_t out_of_order = 0;
    std::uint64_t last_seq = 0;
    std::chrono::microseconds work{0};
};

}  // namespace

int main(int argc, char** argv) {
    using namespace rdb;
    using namespace rdb::bench;

    const long long rate_hz = arg_int(argc, argv, "--rate", 1000);
    const long long count = arg_int(argc, argv, "--count", 100000);
    const long long subscribers = arg_int(argc, argv, "--subscribers", 1);
    const long long payload_bytes = arg_int(argc, argv, "--payload", 1024);
    const long long depth = arg_int(argc, argv, "--depth", 64);
    const long long pool_size = arg_int(argc, argv, "--pool", 256);
    const long long slow_ms = arg_int(argc, argv, "--slow-ms", 0);
    const long long reliable_timeout_ms = arg_int(argc, argv, "--reliable-timeout-ms", 50);
    const bool best_effort = arg_flag(argc, argv, "--best-effort");

    QoS qos;
    qos.reliability = best_effort ? Reliability::BestEffort : Reliability::Reliable;
    qos.depth = static_cast<std::size_t>(depth);

    std::printf("config: rate=%lldHz count=%lld subs=%lld payload=%lldB depth=%lld "
                "reliability=%s slow_ms=%lld\n",
                rate_hz, count, subscribers, payload_bytes, depth, to_string(qos.reliability),
                slow_ms);
    if (slow_ms > 0 && !best_effort) {
        // 这不是 bug，而是 Reliable 的定义：发布者必须等慢订阅者，直到超时才放弃。
        std::printf("warning: Reliable + 慢订阅者 会把发布速率拉到约 %.1f msg/s，"
                    "先用小的 --count 观察背压，再加 --best-effort 对比隔离。\n",
                    1000.0 / static_cast<double>(reliable_timeout_ms));
    }

    auto pool = BufferPool::create(static_cast<std::size_t>(payload_bytes),
                                   static_cast<std::size_t>(pool_size));

    TopicBus bus;
    bus.set_reliable_timeout(std::chrono::milliseconds(reliable_timeout_ms));
    auto publisher = bus.advertise<Sample>("bench", qos);

    std::vector<std::unique_ptr<SubscriberState>> states;
    std::vector<TopicBus::SubscriptionHandle> handles;
    for (long long i = 0; i < subscribers; ++i) {
        states.push_back(std::make_unique<SubscriberState>());
        // 第 0 个订阅者可以被人为拖慢，用来观察隔离效果。
        if (i == 0 && slow_ms > 0) {
            states.back()->work = std::chrono::microseconds(slow_ms * 1000);
        }
        SubscriberState* state = states.back().get();
        handles.push_back(bus.subscribe<Sample>(
            "bench", qos, [state](const std::shared_ptr<const Sample>& message) {
                const std::uint64_t now = now_ns();
                state->latency.record(now - message->publish_ns);
                if (message->seq < state->last_seq) ++state->out_of_order;
                state->last_seq = message->seq;
                ++state->received;
                if (state->work.count() > 0) std::this_thread::sleep_for(state->work);
            }));
    }

    std::uint64_t publish_failed = 0;
    std::uint64_t pool_empty = 0;
    Ticker ticker(static_cast<double>(rate_hz));
    const std::uint64_t start = now_ns();

    for (long long i = 0; i < count; ++i) {
        BufferPtr buffer = pool->acquire();
        if (!buffer) {
            ++pool_empty;  // 池耗尽说明下游还没释放上一批数据，是真实的背压信号
            ticker.sleep_until_next();
            continue;
        }
        buffer->resize(static_cast<std::size_t>(payload_bytes));

        auto message = std::make_shared<Sample>();
        message->seq = static_cast<std::uint64_t>(i);
        message->payload = std::move(buffer);
        message->publish_ns = now_ns();
        if (!publisher.publish(std::shared_ptr<const Sample>(std::move(message)))) ++publish_failed;
        ticker.sleep_until_next();
    }

    const std::uint64_t publish_done = now_ns();
    bus.wait_for_idle(std::chrono::milliseconds(10000));
    bus.shutdown();
    const double seconds = static_cast<double>(publish_done - start) / 1e9;

    std::printf("publish: sent=%lld failed=%llu pool_empty=%llu elapsed=%.3fs rate=%.0f msg/s\n",
                count, static_cast<unsigned long long>(publish_failed),
                static_cast<unsigned long long>(pool_empty), seconds,
                static_cast<double>(count) / (seconds > 0 ? seconds : 1.0));
    std::printf("pool: exhausted=%llu in_use=%zu\n",
                static_cast<unsigned long long>(pool->exhausted_count()), pool->in_use());

    for (std::size_t i = 0; i < states.size(); ++i) {
        const SubscriberStats stats = handles[i].stats();
        std::printf("sub[%zu]: recv=%llu dropped=%llu expired=%llu hwm=%zu out_of_order=%llu\n", i,
                    static_cast<unsigned long long>(stats.delivered),
                    static_cast<unsigned long long>(stats.dropped),
                    static_cast<unsigned long long>(stats.expired), stats.queue_high_water,
                    static_cast<unsigned long long>(states[i]->out_of_order));
        std::printf("sub[%zu]: latency %s\n", i, states[i]->latency.summary("us", 1000.0).c_str());
    }
    return 0;
}
