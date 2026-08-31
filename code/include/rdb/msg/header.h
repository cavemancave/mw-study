// 消息头：定长、小端、显式逐字节编解码。
// 不要直接 memcpy 结构体：编译器 padding、字节序和成员顺序都不是跨平台契约。
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace rdb {

inline constexpr std::uint32_t kMessageMagic = 0x31424452u;  // "RDB1"
inline constexpr std::uint16_t kMessageVersion = 1;
inline constexpr std::size_t kHeaderSize = 48;

enum MessageFlags : std::uint16_t {
    kFlagNone = 0,
    kFlagCompressed = 1u << 0,
    kFlagKeyFrame = 1u << 1,
    kFlagFragment = 1u << 2,
    kFlagLastFragment = 1u << 3,
};

struct MessageHeader {
    std::uint32_t magic = kMessageMagic;
    std::uint16_t version = kMessageVersion;
    std::uint16_t flags = kFlagNone;
    std::uint32_t schema_id = 0;    // 话题上承载的消息类型编号
    std::uint32_t schema_hash = 0;  // 字段布局指纹，用来发现"同名不同结构"
    std::uint64_t seq = 0;          // 每个发布者单调递增，用于检测丢包和重复
    std::uint64_t stamp_ns = 0;     // 采样时刻（传感器打的时间）
    std::uint64_t publish_ns = 0;   // 进入中间件的时刻，两者之差是采集链路延迟
    std::uint32_t payload_size = 0;
    std::uint32_t reserved = 0;
};

namespace detail {

inline void put_u16(std::byte* out, std::uint16_t v) {
    out[0] = static_cast<std::byte>(v & 0xFFu);
    out[1] = static_cast<std::byte>((v >> 8) & 0xFFu);
}

inline void put_u32(std::byte* out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
}

inline void put_u64(std::byte* out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
}

inline std::uint16_t get_u16(const std::byte* in) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[0]) |
                                      static_cast<std::uint16_t>(
                                          static_cast<std::uint16_t>(in[1]) << 8));
}

inline std::uint32_t get_u32(const std::byte* in) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(in[i]) << (8 * i);
    return v;
}

inline std::uint64_t get_u64(const std::byte* in) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(in[i]) << (8 * i);
    return v;
}

}  // namespace detail

inline void encode_header(const MessageHeader& h, std::byte* out) {
    detail::put_u32(out + 0, h.magic);
    detail::put_u16(out + 4, h.version);
    detail::put_u16(out + 6, h.flags);
    detail::put_u32(out + 8, h.schema_id);
    detail::put_u32(out + 12, h.schema_hash);
    detail::put_u64(out + 16, h.seq);
    detail::put_u64(out + 24, h.stamp_ns);
    detail::put_u64(out + 32, h.publish_ns);
    detail::put_u32(out + 40, h.payload_size);
    detail::put_u32(out + 44, h.reserved);
}

// 解码必须校验：魔数错说明流已经错位，版本高于自己说明对端更新。
inline bool decode_header(const std::byte* in, std::size_t size, MessageHeader& out) {
    if (size < kHeaderSize) return false;
    out.magic = detail::get_u32(in + 0);
    if (out.magic != kMessageMagic) return false;
    out.version = detail::get_u16(in + 4);
    if (out.version == 0 || out.version > kMessageVersion) return false;
    out.flags = detail::get_u16(in + 6);
    out.schema_id = detail::get_u32(in + 8);
    out.schema_hash = detail::get_u32(in + 12);
    out.seq = detail::get_u64(in + 16);
    out.stamp_ns = detail::get_u64(in + 24);
    out.publish_ns = detail::get_u64(in + 32);
    out.payload_size = detail::get_u32(in + 40);
    out.reserved = detail::get_u32(in + 44);
    return true;
}

// FNV-1a：给 schema 字符串（字段名+类型拼出来的签名）算指纹。
// 用途是"发现不兼容"，不是安全校验，不要用它做防篡改。
inline constexpr std::uint32_t schema_fingerprint(std::string_view signature) {
    std::uint32_t hash = 2166136261u;
    for (char c : signature) {
        hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        hash *= 16777619u;
    }
    return hash;
}

}  // namespace rdb
