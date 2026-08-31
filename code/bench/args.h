// 压测程序共用的命令行解析：--name value 与 --flag 两种形式。
#pragma once

#include <cstdlib>
#include <cstring>
#include <string>

namespace rdb::bench {

inline long long arg_int(int argc, char** argv, const char* name, long long fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return std::atoll(argv[i + 1]);
    }
    return fallback;
}

inline std::string arg_str(int argc, char** argv, const char* name, const char* fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return std::string(argv[i + 1]);
    }
    return std::string(fallback);
}

inline bool arg_flag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return true;
    }
    return false;
}

}  // namespace rdb::bench
