// 对数分桶直方图（HdrHistogram 简化版）。
// 平均值会掩盖尾延迟，所以全书所有性能结论都必须用 p50/p90/p99/p999 表达。
// 内存固定约 30KB，record() 是 O(1) 且不分配，可以放进热路径。
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <string>
#include <vector>

namespace rdb {

class Histogram {
public:
    // 每个 2 的幂区间再均分 64 份，最大相对误差约 1/128 ≈ 0.8%。
    static constexpr unsigned kSubBits = 6;
    static constexpr std::uint64_t kSubCount = std::uint64_t{1} << kSubBits;
    static constexpr std::size_t kBucketCount =
        static_cast<std::size_t>(kSubCount) * (64 - kSubBits + 1);

    Histogram() : counts_(kBucketCount, 0) {}

    void record(std::uint64_t value) {
        counts_[index_of(value)] += 1;
        ++count_;
        sum_ += value;
        if (value < min_) min_ = value;
        if (value > max_) max_ = value;
    }

    void merge(const Histogram& other) {
        for (std::size_t i = 0; i < kBucketCount; ++i) counts_[i] += other.counts_[i];
        count_ += other.count_;
        sum_ += other.sum_;
        min_ = std::min(min_, other.min_);
        max_ = std::max(max_, other.max_);
    }

    void reset() {
        std::fill(counts_.begin(), counts_.end(), std::uint64_t{0});
        count_ = 0;
        sum_ = 0;
        min_ = UINT64_MAX;
        max_ = 0;
    }

    std::uint64_t count() const { return count_; }
    std::uint64_t min() const { return count_ == 0 ? 0 : min_; }
    std::uint64_t max() const { return max_; }
    double mean() const {
        return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
    }

    // p 取 0..100，返回该分位所在桶的上界（对延迟而言是保守估计）。
    std::uint64_t percentile(double p) const {
        if (count_ == 0) return 0;
        if (p < 0.0) p = 0.0;
        if (p > 100.0) p = 100.0;
        const double target = p / 100.0 * static_cast<double>(count_);
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBucketCount; ++i) {
            seen += counts_[i];
            if (static_cast<double>(seen) >= target) return bucket_upper(i);
        }
        return max_;
    }

    // 单位由调用方决定，只负责换算成同一量纲输出。
    std::string summary(const char* unit = "ns", double scale = 1.0) const {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "n=%llu min=%.2f p50=%.2f p90=%.2f p99=%.2f p99.9=%.2f max=%.2f "
                      "mean=%.2f (%s)",
                      static_cast<unsigned long long>(count_),
                      static_cast<double>(min()) / scale,
                      static_cast<double>(percentile(50)) / scale,
                      static_cast<double>(percentile(90)) / scale,
                      static_cast<double>(percentile(99)) / scale,
                      static_cast<double>(percentile(99.9)) / scale,
                      static_cast<double>(max()) / scale, mean() / scale, unit);
        return std::string(buf);
    }

    static std::size_t index_of(std::uint64_t value) {
        if (value < kSubCount) return static_cast<std::size_t>(value);
        const unsigned exponent = static_cast<unsigned>(std::bit_width(value)) - 1u;
        const unsigned shift = exponent - kSubBits;
        const std::uint64_t sub = (value - (std::uint64_t{1} << exponent)) >> shift;
        return static_cast<std::size_t>(kSubCount +
                                        static_cast<std::uint64_t>(shift) * kSubCount + sub);
    }

    static std::uint64_t bucket_upper(std::size_t index) {
        if (index < static_cast<std::size_t>(kSubCount)) return static_cast<std::uint64_t>(index);
        const std::uint64_t rel = static_cast<std::uint64_t>(index) - kSubCount;
        const unsigned shift = static_cast<unsigned>(rel / kSubCount);
        const std::uint64_t sub = rel % kSubCount;
        const unsigned exponent = shift + kSubBits;
        const std::uint64_t width = std::uint64_t{1} << shift;
        return (std::uint64_t{1} << exponent) + (sub + 1) * width - 1;
    }

private:
    std::vector<std::uint64_t> counts_;
    std::uint64_t count_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t min_ = UINT64_MAX;
    std::uint64_t max_ = 0;
};

}  // namespace rdb
