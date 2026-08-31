#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace rdb {

using Nanos = std::chrono::nanoseconds;

// 单调时钟：测延迟、超时、心跳只能用它，不受 NTP 跳变影响。
inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// 墙上时钟：只用于要和外部系统对齐的时间戳（日志、录制文件头），可能回跳。
inline std::uint64_t wall_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

inline double ns_to_ms(std::uint64_t ns) { return static_cast<double>(ns) / 1e6; }
inline double ns_to_us(std::uint64_t ns) { return static_cast<double>(ns) / 1e3; }

// 定频节拍器：以固定周期唤醒，且不会因为单次处理超时而累积漂移。
class Ticker {
public:
    explicit Ticker(double hz)
        : period_(static_cast<std::int64_t>(1e9 / (hz > 0.0 ? hz : 1.0))),
          next_(std::chrono::steady_clock::now()) {}

    // 返回本次是否发生了追赶（上一轮超时导致丢拍）。
    bool sleep_until_next() {
        next_ += period_;
        const auto now = std::chrono::steady_clock::now();
        if (next_ <= now) {
            next_ = now;  // 追不上就重置基准，避免忙等补拍
            return true;
        }
        std::this_thread::sleep_until(next_);
        return false;
    }

private:
    std::chrono::nanoseconds period_;
    std::chrono::steady_clock::time_point next_;
};

}  // namespace rdb
