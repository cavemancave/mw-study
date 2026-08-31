// 进程内发布订阅总线：中间件的最小可运行内核。
// 三个设计要点：
//   1) 每个订阅者一条队列 + 一个线程，慢消费者只拖垮自己（隔离）。
//   2) QoS 不兼容时不投递，但必须计数并可回调（不静默）。
//   3) 消息以 shared_ptr 扇出，同机多订阅者不复制负载（零拷贝的进程内版本）。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rdb/bus/qos.h"
#include "rdb/common/time.h"
#include "rdb/concurrency/bounded_queue.h"

namespace rdb {

struct SubscriberStats {
    std::uint64_t delivered = 0;
    std::uint64_t dropped = 0;           // 队列溢出丢弃
    std::uint64_t expired = 0;           // 超过 lifespan，取出时已过期
    std::uint64_t deadline_missed = 0;   // 相邻两次投递间隔超过 deadline
    std::uint64_t qos_rejected = 0;      // 因 QoS 不兼容未投递
    std::size_t queue_high_water = 0;
};

namespace detail {

struct Envelope {
    std::shared_ptr<const void> payload;
    std::uint64_t seq = 0;
    std::uint64_t publish_ns = 0;
};

struct Subscription {
    Subscription(std::string topic_name, std::type_index type_index, const QoS& qos_in)
        : topic(std::move(topic_name)),
          type(type_index),
          qos(qos_in),
          queue(qos_in.depth, qos_in.reliability == Reliability::Reliable
                                  ? OverflowPolicy::Block
                                  : OverflowPolicy::DropOldest) {}

    std::string topic;
    std::type_index type;
    QoS qos;
    BoundedQueue<Envelope> queue;
    std::function<void(const std::shared_ptr<const void>&)> callback;
    std::thread worker;
    std::atomic<std::uint64_t> delivered{0};
    std::atomic<std::uint64_t> expired{0};
    std::atomic<std::uint64_t> deadline_missed{0};
    std::atomic<std::uint64_t> qos_rejected{0};
    std::atomic<std::uint64_t> last_delivery_ns{0};
    std::atomic<bool> mismatch_reported{false};
};

}  // namespace detail

class TopicBus {
public:
    using QosMismatchHandler = std::function<void(const std::string& topic, QosMismatch)>;

    TopicBus() = default;
    ~TopicBus() { shutdown(); }
    TopicBus(const TopicBus&) = delete;
    TopicBus& operator=(const TopicBus&) = delete;

    class SubscriptionHandle {
    public:
        SubscriptionHandle() = default;
        bool valid() const { return static_cast<bool>(sub_); }

        SubscriberStats stats() const {
            SubscriberStats s;
            if (!sub_) return s;
            s.delivered = sub_->delivered.load(std::memory_order_relaxed);
            s.dropped = sub_->queue.dropped();
            s.expired = sub_->expired.load(std::memory_order_relaxed);
            s.deadline_missed = sub_->deadline_missed.load(std::memory_order_relaxed);
            s.qos_rejected = sub_->qos_rejected.load(std::memory_order_relaxed);
            s.queue_high_water = sub_->queue.high_water();
            return s;
        }

        std::size_t queue_size() const { return sub_ ? sub_->queue.size() : 0; }

    private:
        friend class TopicBus;
        explicit SubscriptionHandle(std::shared_ptr<detail::Subscription> sub)
            : sub_(std::move(sub)) {}
        std::shared_ptr<detail::Subscription> sub_;
    };

    template <typename T>
    class Publisher {
    public:
        Publisher() = default;

        bool publish(std::shared_ptr<const T> message) const {
            return bus_ != nullptr && bus_->publish_erased(topic_, std::type_index(typeid(T)),
                                                           qos_, std::move(message));
        }

        bool publish(T value) const {
            return publish(std::make_shared<const T>(std::move(value)));
        }

        const std::string& topic() const { return topic_; }
        const QoS& qos() const { return qos_; }
        bool valid() const { return bus_ != nullptr; }
        std::size_t subscriber_count() const {
            return bus_ == nullptr ? 0 : bus_->subscriber_count(topic_);
        }

    private:
        friend class TopicBus;
        Publisher(TopicBus* bus, std::string topic_name, const QoS& qos_in)
            : bus_(bus), topic_(std::move(topic_name)), qos_(qos_in) {}

        TopicBus* bus_ = nullptr;
        std::string topic_;
        QoS qos_;
    };

    template <typename T>
    Publisher<T> advertise(const std::string& topic, const QoS& qos = QoS()) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        ensure_topic(topic, std::type_index(typeid(T)));
        return Publisher<T>(this, topic, qos);
    }

    template <typename T>
    SubscriptionHandle subscribe(const std::string& topic, const QoS& qos,
                                 std::function<void(const std::shared_ptr<const T>&)> callback) {
        auto sub = std::make_shared<detail::Subscription>(topic, std::type_index(typeid(T)), qos);
        sub->callback = [cb = std::move(callback)](const std::shared_ptr<const void>& payload) {
            cb(std::static_pointer_cast<const T>(payload));
        };

        std::vector<detail::Envelope> backlog;
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            TopicEntry& entry = ensure_topic(topic, std::type_index(typeid(T)));
            if (entry.type != sub->type) return SubscriptionHandle{};  // 同名话题类型冲突
            entry.subs.push_back(sub);
            if (qos.durability == Durability::TransientLocal) {
                backlog.assign(entry.latched.begin(), entry.latched.end());
            }
        }

        // 晚加入的订阅者先补历史数据，再启动线程，保证顺序不乱。
        for (detail::Envelope& env : backlog) sub->queue.try_push(std::move(env));

        detail::Subscription* raw = sub.get();
        sub->worker = std::thread([this, raw] { deliver_loop(raw); });
        return SubscriptionHandle(sub);
    }

    void unsubscribe(SubscriptionHandle& handle) {
        std::shared_ptr<detail::Subscription> sub = handle.sub_;
        if (!sub) return;
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            auto it = topics_.find(sub->topic);
            if (it != topics_.end()) {
                std::vector<std::shared_ptr<detail::Subscription>>& subs = it->second.subs;
                for (std::size_t i = 0; i < subs.size(); ++i) {
                    if (subs[i] == sub) {
                        subs.erase(subs.begin() + static_cast<std::ptrdiff_t>(i));
                        break;
                    }
                }
            }
        }
        stop_subscription(*sub);
        handle.sub_.reset();
    }

    // 关闭所有订阅者：先摘链再关队列，保证不会有新消息进来。
    void shutdown() {
        std::vector<std::shared_ptr<detail::Subscription>> all;
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            for (auto& kv : topics_) {
                for (auto& sub : kv.second.subs) all.push_back(sub);
                kv.second.subs.clear();
                kv.second.latched.clear();
            }
        }
        for (auto& sub : all) stop_subscription(*sub);
    }

    std::size_t subscriber_count(const std::string& topic) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = topics_.find(topic);
        return it == topics_.end() ? 0 : it->second.subs.size();
    }

    std::uint64_t published_count(const std::string& topic) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = topics_.find(topic);
        return it == topics_.end() ? 0 : it->second.published;
    }

    std::uint64_t qos_mismatch_count() const {
        return qos_mismatches_.load(std::memory_order_relaxed);
    }

    std::uint64_t type_mismatch_count() const {
        return type_mismatches_.load(std::memory_order_relaxed);
    }

    void set_qos_mismatch_handler(QosMismatchHandler handler) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        mismatch_handler_ = std::move(handler);
    }

    // Reliable 发布在队列满时最多阻塞这么久，超时按发布失败处理。
    // 没有超时的"可靠"会让一个卡死的订阅者拖垮整条发布链路。
    void set_reliable_timeout(std::chrono::nanoseconds timeout) { reliable_timeout_ = timeout; }

    // 测试与演示用：等待所有订阅队列排空。
    bool wait_for_idle(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            bool idle = true;
            {
                std::shared_lock<std::shared_mutex> lock(mutex_);
                for (const auto& kv : topics_) {
                    for (const auto& sub : kv.second.subs) {
                        if (!sub->queue.empty()) {
                            idle = false;
                            break;
                        }
                    }
                    if (!idle) break;
                }
            }
            if (idle) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

private:
    struct TopicEntry {
        explicit TopicEntry(std::type_index t) : type(t) {}
        std::type_index type;
        std::vector<std::shared_ptr<detail::Subscription>> subs;
        std::deque<detail::Envelope> latched;
        std::uint64_t published = 0;
    };

    TopicEntry& ensure_topic(const std::string& topic, std::type_index type) {
        auto it = topics_.find(topic);
        if (it == topics_.end()) {
            it = topics_.emplace(topic, TopicEntry(type)).first;
        }
        return it->second;
    }

    bool publish_erased(const std::string& topic, std::type_index type, const QoS& pub_qos,
                        std::shared_ptr<const void> payload) {
        std::vector<std::shared_ptr<detail::Subscription>> targets;
        detail::Envelope envelope;
        envelope.payload = std::move(payload);
        envelope.publish_ns = now_ns();

        QosMismatchHandler handler;
        std::vector<QosMismatch> mismatches;
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            auto it = topics_.find(topic);
            if (it == topics_.end()) return false;
            TopicEntry& entry = it->second;
            if (entry.type != type) {
                type_mismatches_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            envelope.seq = ++entry.published;

            if (pub_qos.durability == Durability::TransientLocal) {
                entry.latched.push_back(envelope);
                while (entry.latched.size() > pub_qos.depth) entry.latched.pop_front();
            }

            targets.reserve(entry.subs.size());
            for (const std::shared_ptr<detail::Subscription>& sub : entry.subs) {
                const QosMismatch m = check_compatible(pub_qos, sub->qos);
                if (m != QosMismatch::None) {
                    sub->qos_rejected.fetch_add(1, std::memory_order_relaxed);
                    qos_mismatches_.fetch_add(1, std::memory_order_relaxed);
                    bool reported = false;
                    if (sub->mismatch_reported.compare_exchange_strong(reported, true)) {
                        mismatches.push_back(m);
                    }
                    continue;
                }
                targets.push_back(sub);
            }
            handler = mismatch_handler_;
        }

        for (QosMismatch m : mismatches) {
            if (handler) handler(topic, m);
        }

        // 投递在锁外完成：Reliable 满队列会阻塞，绝不能持锁等待。
        bool all_accepted = true;
        for (const std::shared_ptr<detail::Subscription>& sub : targets) {
            PushStatus status;
            if (sub->qos.reliability == Reliability::Reliable) {
                status = sub->queue.push_for(envelope, reliable_timeout_);
            } else {
                status = sub->queue.push(envelope);  // BestEffort 队列策略是丢旧，不会阻塞
            }
            if (status != PushStatus::Ok && status != PushStatus::DroppedOldest) {
                all_accepted = false;
            }
        }
        return all_accepted;
    }

    void deliver_loop(detail::Subscription* sub) {
        for (;;) {
            std::optional<detail::Envelope> envelope = sub->queue.pop();
            if (!envelope) return;  // 队列已关闭且取空

            const std::uint64_t now = now_ns();
            const std::uint64_t lifespan =
                static_cast<std::uint64_t>(sub->qos.lifespan.count() > 0 ? sub->qos.lifespan.count()
                                                                         : 0);
            if (lifespan > 0 && now > envelope->publish_ns &&
                now - envelope->publish_ns > lifespan) {
                sub->expired.fetch_add(1, std::memory_order_relaxed);
                continue;  // 过期数据投递出去只会误导下游
            }

            if (sub->qos.deadline.count() > 0) {
                const std::uint64_t last = sub->last_delivery_ns.load(std::memory_order_relaxed);
                const std::uint64_t budget = static_cast<std::uint64_t>(sub->qos.deadline.count());
                if (last != 0 && now - last > budget) {
                    sub->deadline_missed.fetch_add(1, std::memory_order_relaxed);
                }
            }
            sub->last_delivery_ns.store(now, std::memory_order_relaxed);

            sub->callback(envelope->payload);
            sub->delivered.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static void stop_subscription(detail::Subscription& sub) {
        sub.queue.close();
        if (sub.worker.joinable()) sub.worker.join();
    }

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, TopicEntry> topics_;
    QosMismatchHandler mismatch_handler_;
    std::atomic<std::uint64_t> qos_mismatches_{0};
    std::atomic<std::uint64_t> type_mismatches_{0};
    std::chrono::nanoseconds reliable_timeout_{std::chrono::milliseconds(100)};
};

}  // namespace rdb
