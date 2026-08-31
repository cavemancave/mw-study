#include "rdb/bus/qos.h"

#include <chrono>

#include "rdb/testing/check.h"

using namespace rdb;
using namespace std::chrono_literals;

RDB_TEST(qos_reliability_follows_requested_offered_rule) {
    QoS reliable;
    reliable.reliability = Reliability::Reliable;
    QoS best_effort;
    best_effort.reliability = Reliability::BestEffort;

    // 发布者提供的强度必须不低于订阅者请求的强度。
    RDB_CHECK(check_compatible(reliable, reliable) == QosMismatch::None);
    RDB_CHECK(check_compatible(reliable, best_effort) == QosMismatch::None);
    RDB_CHECK(check_compatible(best_effort, best_effort) == QosMismatch::None);
    RDB_CHECK(check_compatible(best_effort, reliable) == QosMismatch::Reliability);
}

RDB_TEST(qos_durability_follows_requested_offered_rule) {
    QoS transient = QoS::latched();
    QoS volatile_qos;
    volatile_qos.durability = Durability::Volatile;

    RDB_CHECK(check_compatible(transient, volatile_qos) == QosMismatch::None);
    RDB_CHECK(check_compatible(transient, transient) == QosMismatch::None);
    RDB_CHECK(check_compatible(volatile_qos, transient) == QosMismatch::Durability);
}

RDB_TEST(qos_deadline_requires_publisher_to_be_at_least_as_fast) {
    QoS fast;
    fast.deadline = 10ms;
    QoS slow;
    slow.deadline = 100ms;
    QoS none;

    RDB_CHECK(check_compatible(fast, slow) == QosMismatch::None);   // 10ms 一帧满足 100ms 要求
    RDB_CHECK(check_compatible(slow, fast) == QosMismatch::Deadline);
    RDB_CHECK(check_compatible(none, fast) == QosMismatch::Deadline);  // 未承诺周期
    RDB_CHECK(check_compatible(none, none) == QosMismatch::None);
}

RDB_TEST(qos_presets_have_expected_shape) {
    RDB_CHECK(QoS::sensor_data().reliability == Reliability::BestEffort);
    RDB_CHECK(QoS::command().reliability == Reliability::Reliable);
    RDB_CHECK(QoS::latched().durability == Durability::TransientLocal);
    RDB_CHECK_EQ(QoS::latched(3).depth, std::size_t{3});
}
