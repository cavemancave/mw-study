// QoS 策略与兼容性判定。
// 采用 DDS 的 RxO（Requested / Offered）思路：发布者"提供"的强度必须不低于订阅者"请求"的强度。
#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace rdb {

enum class Reliability {
    BestEffort,  // 发出去就算完；丢了不补
    Reliable,    // 保证送达（本进程内表现为满队列阻塞或超时）
};

enum class Durability {
    Volatile,        // 只有订阅之后发布的数据才收得到
    TransientLocal,  // 发布者保留最近 depth 条，晚加入的订阅者能补齐
};

enum class HistoryKind {
    KeepLast,  // 只保留最近 depth 条
    KeepAll,   // 保留全部（仍受 depth 上限约束，否则就是无界队列）
};

struct QoS {
    Reliability reliability = Reliability::Reliable;
    Durability durability = Durability::Volatile;
    HistoryKind history = HistoryKind::KeepLast;
    std::size_t depth = 10;
    // 0 表示不启用。deadline 是"两条消息之间允许的最大间隔"。
    std::chrono::nanoseconds deadline{0};
    // 0 表示不启用。lifespan 是"消息发布后多久失效"，过期数据不再投递。
    std::chrono::nanoseconds lifespan{0};

    static QoS sensor_data(std::size_t depth_ = 5) {
        QoS q;
        q.reliability = Reliability::BestEffort;
        q.durability = Durability::Volatile;
        q.depth = depth_;
        return q;
    }

    static QoS command(std::size_t depth_ = 32) {
        QoS q;
        q.reliability = Reliability::Reliable;
        q.durability = Durability::Volatile;
        q.depth = depth_;
        return q;
    }

    static QoS latched(std::size_t depth_ = 1) {
        QoS q;
        q.reliability = Reliability::Reliable;
        q.durability = Durability::TransientLocal;
        q.depth = depth_;
        return q;
    }
};

enum class QosMismatch {
    None,
    Reliability,  // 发布者 BestEffort，订阅者要求 Reliable
    Durability,   // 发布者 Volatile，订阅者要求 TransientLocal
    Deadline,     // 发布者承诺的周期长于订阅者能接受的
};

// 不兼容时静默不匹配是最难排查的问题之一：中间件必须把结果暴露成事件或日志。
inline QosMismatch check_compatible(const QoS& offered, const QoS& requested) {
    if (offered.reliability == Reliability::BestEffort &&
        requested.reliability == Reliability::Reliable) {
        return QosMismatch::Reliability;
    }
    if (offered.durability == Durability::Volatile &&
        requested.durability == Durability::TransientLocal) {
        return QosMismatch::Durability;
    }
    if (requested.deadline.count() > 0) {
        if (offered.deadline.count() == 0 || offered.deadline > requested.deadline) {
            return QosMismatch::Deadline;
        }
    }
    return QosMismatch::None;
}

inline const char* to_string(QosMismatch m) {
    switch (m) {
        case QosMismatch::None: return "none";
        case QosMismatch::Reliability: return "reliability";
        case QosMismatch::Durability: return "durability";
        case QosMismatch::Deadline: return "deadline";
    }
    return "unknown";
}

inline const char* to_string(Reliability r) {
    return r == Reliability::Reliable ? "reliable" : "best_effort";
}

inline const char* to_string(Durability d) {
    return d == Durability::TransientLocal ? "transient_local" : "volatile";
}

}  // namespace rdb
