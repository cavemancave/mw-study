// 极简测试框架：不引入外部依赖，保证 clone 后可直接 cmake + ctest。
#pragma once

#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>
#include <vector>

namespace rdb::testing {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back(TestCase{name, fn}); }
};

struct AssertionFailure : std::exception {
    std::string message;
    explicit AssertionFailure(std::string m) : message(std::move(m)) {}
    const char* what() const noexcept override { return message.c_str(); }
};

inline std::string location(const char* file, int line) {
    return std::string(file) + ":" + std::to_string(line);
}

// 轮询等待并发条件，避免用固定 sleep 写出偶发失败的测试。
template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

inline int run_all() {
    int failed = 0;
    for (const TestCase& tc : registry()) {
        try {
            tc.fn();
            std::printf("[  PASS  ] %s\n", tc.name);
        } catch (const AssertionFailure& e) {
            std::printf("[  FAIL  ] %s\n           %s\n", tc.name, e.what());
            ++failed;
        } catch (const std::exception& e) {
            std::printf("[ THROW  ] %s\n           %s\n", tc.name, e.what());
            ++failed;
        }
    }
    std::printf("%zu tests, %d failed\n", registry().size(), failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace rdb::testing

#define RDB_TEST(name)                                                            \
    static void name();                                                           \
    static const ::rdb::testing::Registrar rdb_registrar_##name(#name, &name);    \
    static void name()

#define RDB_CHECK(cond)                                                           \
    do {                                                                          \
        if (!(cond)) {                                                            \
            throw ::rdb::testing::AssertionFailure(                               \
                ::rdb::testing::location(__FILE__, __LINE__) +                    \
                " expected true: " #cond);                                        \
        }                                                                         \
    } while (false)

#define RDB_CHECK_EQ(lhs, rhs)                                                    \
    do {                                                                          \
        const auto rdb_lhs_ = (lhs);                                              \
        const auto rdb_rhs_ = (rhs);                                              \
        if (!(rdb_lhs_ == rdb_rhs_)) {                                            \
            throw ::rdb::testing::AssertionFailure(                               \
                ::rdb::testing::location(__FILE__, __LINE__) +                    \
                " expected " #lhs " == " #rhs);                                   \
        }                                                                         \
    } while (false)

#define RDB_CHECK_LT(lhs, rhs)                                                    \
    do {                                                                          \
        if (!((lhs) < (rhs))) {                                                   \
            throw ::rdb::testing::AssertionFailure(                               \
                ::rdb::testing::location(__FILE__, __LINE__) +                    \
                " expected " #lhs " < " #rhs);                                    \
        }                                                                         \
    } while (false)

#define RDB_CHECK_GE(lhs, rhs)                                                    \
    do {                                                                          \
        if (!((lhs) >= (rhs))) {                                                  \
            throw ::rdb::testing::AssertionFailure(                               \
                ::rdb::testing::location(__FILE__, __LINE__) +                    \
                " expected " #lhs " >= " #rhs);                                   \
        }                                                                         \
    } while (false)
