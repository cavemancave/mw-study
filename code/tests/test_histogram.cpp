#include "rdb/metrics/histogram.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "rdb/testing/check.h"

using namespace rdb;

RDB_TEST(histogram_small_values_are_exact) {
    Histogram h;
    for (std::uint64_t v = 0; v < Histogram::kSubCount; ++v) {
        RDB_CHECK_EQ(Histogram::index_of(v), static_cast<std::size_t>(v));
        RDB_CHECK_EQ(Histogram::bucket_upper(Histogram::index_of(v)), v);
    }
}

// 每个值必须落进"上界不小于自身"的桶，且相对误差在设计精度内。
RDB_TEST(histogram_bucket_bounds_are_consistent) {
    for (unsigned exp = 6; exp < 63; ++exp) {
        const std::uint64_t base = std::uint64_t{1} << exp;
        const std::uint64_t samples[] = {base, base + 1, base + (base / 3), 2 * base - 1};
        for (std::uint64_t v : samples) {
            const std::size_t idx = Histogram::index_of(v);
            RDB_CHECK(idx < Histogram::kBucketCount);
            const std::uint64_t upper = Histogram::bucket_upper(idx);
            RDB_CHECK(upper >= v);
            const double error = static_cast<double>(upper - v) / static_cast<double>(v);
            RDB_CHECK(error < 0.02);
        }
    }
}

RDB_TEST(histogram_percentile_matches_sorted_samples) {
    Histogram h;
    std::vector<std::uint64_t> samples;
    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<std::uint64_t> dist(1000, 500000);
    for (int i = 0; i < 100000; ++i) {
        const std::uint64_t v = dist(rng);
        samples.push_back(v);
        h.record(v);
    }
    std::sort(samples.begin(), samples.end());

    RDB_CHECK_EQ(h.count(), static_cast<std::uint64_t>(samples.size()));
    RDB_CHECK_EQ(h.min(), samples.front());
    RDB_CHECK_EQ(h.max(), samples.back());

    const double percentiles[] = {50.0, 90.0, 99.0, 99.9};
    for (double p : percentiles) {
        const std::size_t rank =
            static_cast<std::size_t>(p / 100.0 * static_cast<double>(samples.size()));
        const std::uint64_t exact = samples[std::min(rank, samples.size() - 1)];
        const std::uint64_t got = h.percentile(p);
        const double error =
            static_cast<double>(got > exact ? got - exact : exact - got) /
            static_cast<double>(exact);
        RDB_CHECK(error < 0.02);
    }
}

RDB_TEST(histogram_merge_combines_counts) {
    Histogram a;
    Histogram b;
    for (std::uint64_t v = 1; v <= 1000; ++v) a.record(v);
    for (std::uint64_t v = 1001; v <= 2000; ++v) b.record(v);
    a.merge(b);
    RDB_CHECK_EQ(a.count(), std::uint64_t{2000});
    RDB_CHECK_EQ(a.min(), std::uint64_t{1});
    RDB_CHECK(a.max() >= 2000);
    RDB_CHECK(a.mean() > 900.0 && a.mean() < 1100.0);
}

RDB_TEST(histogram_reset_clears_state) {
    Histogram h;
    h.record(42);
    h.reset();
    RDB_CHECK_EQ(h.count(), std::uint64_t{0});
    RDB_CHECK_EQ(h.percentile(99), std::uint64_t{0});
}
