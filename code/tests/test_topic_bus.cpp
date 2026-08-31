#include "rdb/bus/topic_bus.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rdb/testing/check.h"

using namespace rdb;
using namespace std::chrono_literals;
using rdb::testing::wait_for;

namespace {

struct ImuSample {
    std::uint64_t seq = 0;
    double gyro_z = 0.0;
};

}  // namespace

RDB_TEST(topic_bus_delivers_to_every_subscriber) {
    TopicBus bus;
    auto publisher = bus.advertise<ImuSample>("imu", QoS::command());

    std::atomic<int> a{0};
    std::atomic<int> b{0};
    auto sub_a = bus.subscribe<ImuSample>(
        "imu", QoS::command(256), [&a](const std::shared_ptr<const ImuSample>&) { ++a; });
    auto sub_b = bus.subscribe<ImuSample>(
        "imu", QoS::command(256), [&b](const std::shared_ptr<const ImuSample>&) { ++b; });
    RDB_CHECK_EQ(publisher.subscriber_count(), std::size_t{2});

    for (int i = 0; i < 100; ++i) {
        RDB_CHECK(publisher.publish(ImuSample{static_cast<std::uint64_t>(i), 0.5}));
    }
    RDB_CHECK(wait_for([&] { return a.load() == 100 && b.load() == 100; }));
    RDB_CHECK_EQ(sub_a.stats().delivered, std::uint64_t{100});
    RDB_CHECK_EQ(sub_b.stats().dropped, std::uint64_t{0});
}

// 扇出不复制负载：所有订阅者拿到的是同一个对象地址。
RDB_TEST(topic_bus_fanout_shares_payload) {
    TopicBus bus;
    auto publisher = bus.advertise<ImuSample>("imu", QoS::command());
    std::atomic<const ImuSample*> seen_a{nullptr};
    std::atomic<const ImuSample*> seen_b{nullptr};

    auto sub_a = bus.subscribe<ImuSample>(
        "imu", QoS::command(),
        [&seen_a](const std::shared_ptr<const ImuSample>& m) { seen_a.store(m.get()); });
    auto sub_b = bus.subscribe<ImuSample>(
        "imu", QoS::command(),
        [&seen_b](const std::shared_ptr<const ImuSample>& m) { seen_b.store(m.get()); });

    auto message = std::make_shared<const ImuSample>(ImuSample{1, 0.25});
    RDB_CHECK(publisher.publish(message));
    RDB_CHECK(wait_for([&] { return seen_a.load() != nullptr && seen_b.load() != nullptr; }));
    RDB_CHECK(seen_a.load() == message.get());
    RDB_CHECK(seen_b.load() == message.get());
}

// 核心结论：慢消费者只拖垮自己的队列，既不阻塞发布者，也不影响其他订阅者。
RDB_TEST(topic_bus_slow_subscriber_does_not_block_others) {
    TopicBus bus;
    auto publisher = bus.advertise<ImuSample>("image", QoS::sensor_data(4));

    std::atomic<int> slow_count{0};
    std::atomic<int> fast_count{0};
    auto slow = bus.subscribe<ImuSample>(
        "image", QoS::sensor_data(4), [&slow_count](const std::shared_ptr<const ImuSample>&) {
            std::this_thread::sleep_for(20ms);
            ++slow_count;
        });
    auto fast = bus.subscribe<ImuSample>(
        "image", QoS::sensor_data(64),
        [&fast_count](const std::shared_ptr<const ImuSample>&) { ++fast_count; });

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) publisher.publish(ImuSample{static_cast<std::uint64_t>(i), 0.0});
    const auto publish_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    // 慢订阅者串行处理 50 条需要 1 秒以上，发布侧必须远快于此。
    RDB_CHECK(publish_ms < 300ms);
    RDB_CHECK(wait_for([&] { return fast_count.load() == 50; }));
    RDB_CHECK(slow.stats().dropped > 0);
    RDB_CHECK_EQ(fast.stats().dropped, std::uint64_t{0});
}

RDB_TEST(topic_bus_reliable_publish_times_out_instead_of_hanging) {
    TopicBus bus;
    bus.set_reliable_timeout(30ms);
    QoS qos = QoS::command(2);
    auto publisher = bus.advertise<ImuSample>("cmd", qos);

    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    auto sub = bus.subscribe<ImuSample>("cmd", qos,
                                        [&entered, &release](const std::shared_ptr<const ImuSample>&) {
                                            entered.store(true);
                                            while (!release.load()) std::this_thread::sleep_for(1ms);
                                        });

    RDB_CHECK(publisher.publish(ImuSample{1, 0.0}));
    RDB_CHECK(wait_for([&] { return entered.load(); }));  // 订阅线程已卡在回调里
    RDB_CHECK(publisher.publish(ImuSample{2, 0.0}));      // 入队
    RDB_CHECK(publisher.publish(ImuSample{3, 0.0}));      // 队列满

    const auto start = std::chrono::steady_clock::now();
    const bool accepted = publisher.publish(ImuSample{4, 0.0});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    RDB_CHECK(!accepted);
    RDB_CHECK(elapsed >= 20ms);
    RDB_CHECK(elapsed < 500ms);

    release.store(true);
    bus.shutdown();
}

RDB_TEST(topic_bus_transient_local_serves_late_joiner) {
    TopicBus bus;
    auto publisher = bus.advertise<ImuSample>("map", QoS::latched(3));
    for (int i = 0; i < 5; ++i) publisher.publish(ImuSample{static_cast<std::uint64_t>(i), 0.0});

    std::vector<std::uint64_t> late_seen;
    std::mutex late_mutex;
    auto late = bus.subscribe<ImuSample>(
        "map", QoS::latched(10), [&](const std::shared_ptr<const ImuSample>& m) {
            std::lock_guard<std::mutex> lock(late_mutex);
            late_seen.push_back(m->seq);
        });

    std::atomic<int> volatile_count{0};
    auto fresh = bus.subscribe<ImuSample>(
        "map", QoS::command(),
        [&volatile_count](const std::shared_ptr<const ImuSample>&) { ++volatile_count; });

    // 发布者 depth=3，晚加入者只补得到最近 3 条（seq 2/3/4），不是全部 5 条。
    RDB_CHECK(wait_for([&] {
        std::lock_guard<std::mutex> lock(late_mutex);
        return late_seen.size() == 3;
    }));
    RDB_CHECK_EQ(volatile_count.load(), 0);  // Volatile 订阅者拿不到历史数据

    publisher.publish(ImuSample{99, 0.0});
    RDB_CHECK(wait_for([&] { return volatile_count.load() == 1; }));
    RDB_CHECK(wait_for([&] {
        std::lock_guard<std::mutex> lock(late_mutex);
        return late_seen.size() == 4 && late_seen[0] == 2 && late_seen[3] == 99;
    }));
    RDB_CHECK(fresh.valid());
}

// QoS 不兼容必须计数并可回调，绝不能静默丢弃。
RDB_TEST(topic_bus_qos_mismatch_is_reported_not_silent) {
    TopicBus bus;
    std::atomic<int> mismatch_events{0};
    bus.set_qos_mismatch_handler(
        [&mismatch_events](const std::string&, QosMismatch m) {
            if (m == QosMismatch::Reliability) ++mismatch_events;
        });

    auto publisher = bus.advertise<ImuSample>("lidar", QoS::sensor_data());
    std::atomic<int> got{0};
    auto sub = bus.subscribe<ImuSample>(
        "lidar", QoS::command(), [&got](const std::shared_ptr<const ImuSample>&) { ++got; });

    for (int i = 0; i < 10; ++i) publisher.publish(ImuSample{static_cast<std::uint64_t>(i), 0.0});
    RDB_CHECK(wait_for([&] { return sub.stats().qos_rejected == 10; }));
    RDB_CHECK_EQ(got.load(), 0);
    RDB_CHECK_EQ(bus.qos_mismatch_count(), std::uint64_t{10});
    RDB_CHECK_EQ(mismatch_events.load(), 1);  // 只回调一次，避免刷屏
}

RDB_TEST(topic_bus_rejects_type_mismatch_on_same_topic) {
    TopicBus bus;
    auto int_publisher = bus.advertise<int>("shared", QoS::command());
    auto bad_sub = bus.subscribe<double>("shared", QoS::command(),
                                         [](const std::shared_ptr<const double>&) {});
    RDB_CHECK(!bad_sub.valid());

    auto bad_publisher = bus.advertise<double>("shared", QoS::command());
    RDB_CHECK(!bad_publisher.publish(1.5));
    RDB_CHECK_EQ(bus.type_mismatch_count(), std::uint64_t{1});
    RDB_CHECK(int_publisher.publish(7));
}

RDB_TEST(topic_bus_unsubscribe_stops_delivery) {
    TopicBus bus;
    auto publisher = bus.advertise<ImuSample>("odom", QoS::command());
    std::atomic<int> count{0};
    auto sub = bus.subscribe<ImuSample>(
        "odom", QoS::command(), [&count](const std::shared_ptr<const ImuSample>&) { ++count; });

    publisher.publish(ImuSample{1, 0.0});
    RDB_CHECK(wait_for([&] { return count.load() == 1; }));

    bus.unsubscribe(sub);
    RDB_CHECK(!sub.valid());
    RDB_CHECK_EQ(publisher.subscriber_count(), std::size_t{0});

    publisher.publish(ImuSample{2, 0.0});
    std::this_thread::sleep_for(30ms);
    RDB_CHECK_EQ(count.load(), 1);
}

// lifespan 让排队太久的数据在投递前就作废：过期的控制指令比没有指令更危险。
RDB_TEST(topic_bus_lifespan_drops_stale_messages) {
    TopicBus bus;
    QoS pub_qos = QoS::command(32);
    auto publisher = bus.advertise<ImuSample>("stale", pub_qos);

    QoS sub_qos = QoS::command(32);
    sub_qos.lifespan = 5ms;
    std::atomic<bool> release{false};
    std::atomic<int> delivered{0};
    auto sub = bus.subscribe<ImuSample>("stale", sub_qos,
                                        [&](const std::shared_ptr<const ImuSample>&) {
                                            if (delivered.fetch_add(1) == 0) {
                                                while (!release.load()) {
                                                    std::this_thread::sleep_for(1ms);
                                                }
                                            }
                                        });

    for (int i = 0; i < 10; ++i) publisher.publish(ImuSample{static_cast<std::uint64_t>(i), 0.0});
    std::this_thread::sleep_for(60ms);
    release.store(true);

    RDB_CHECK(wait_for([&] { return sub.stats().expired == 9; }));
    RDB_CHECK_EQ(delivered.load(), 1);
}
