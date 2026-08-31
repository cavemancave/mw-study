#include "rdb/msg/header.h"

#include <array>
#include <cstddef>

#include "rdb/testing/check.h"

using namespace rdb;

RDB_TEST(header_round_trip) {
    MessageHeader in;
    in.flags = kFlagKeyFrame | kFlagCompressed;
    in.schema_id = 7;
    in.schema_hash = schema_fingerprint("ImuSample:u64 stamp_ns,f64[3] gyro,f64[3] accel");
    in.seq = 0x0102030405060708ull;
    in.stamp_ns = 1234567890123ull;
    in.publish_ns = 1234567890999ull;
    in.payload_size = 4096;

    std::array<std::byte, kHeaderSize> wire{};
    encode_header(in, wire.data());

    MessageHeader out;
    RDB_CHECK(decode_header(wire.data(), wire.size(), out));
    RDB_CHECK_EQ(out.magic, kMessageMagic);
    RDB_CHECK_EQ(out.version, kMessageVersion);
    RDB_CHECK_EQ(out.flags, in.flags);
    RDB_CHECK_EQ(out.schema_id, in.schema_id);
    RDB_CHECK_EQ(out.schema_hash, in.schema_hash);
    RDB_CHECK_EQ(out.seq, in.seq);
    RDB_CHECK_EQ(out.stamp_ns, in.stamp_ns);
    RDB_CHECK_EQ(out.publish_ns, in.publish_ns);
    RDB_CHECK_EQ(out.payload_size, in.payload_size);
}

// 线上字节序必须固定：换一台大端机器不能改变消息格式。
RDB_TEST(header_is_little_endian_on_the_wire) {
    MessageHeader in;
    in.seq = 0x0000000000000001ull;
    std::array<std::byte, kHeaderSize> wire{};
    encode_header(in, wire.data());
    RDB_CHECK_EQ(static_cast<unsigned>(wire[16]), 1u);
    RDB_CHECK_EQ(static_cast<unsigned>(wire[17]), 0u);
    // 魔数在流里应当可读为 "RDB1"
    RDB_CHECK_EQ(static_cast<char>(wire[0]), 'R');
    RDB_CHECK_EQ(static_cast<char>(wire[1]), 'D');
    RDB_CHECK_EQ(static_cast<char>(wire[2]), 'B');
    RDB_CHECK_EQ(static_cast<char>(wire[3]), '1');
}

RDB_TEST(header_rejects_bad_input) {
    std::array<std::byte, kHeaderSize> wire{};
    MessageHeader out;

    RDB_CHECK(!decode_header(wire.data(), kHeaderSize - 1, out));  // 截断
    RDB_CHECK(!decode_header(wire.data(), kHeaderSize, out));      // 魔数为 0

    MessageHeader in;
    encode_header(in, wire.data());
    wire[4] = static_cast<std::byte>(99);  // 版本高于本端能理解的
    RDB_CHECK(!decode_header(wire.data(), kHeaderSize, out));
}

RDB_TEST(schema_fingerprint_detects_layout_change) {
    const std::uint32_t a = schema_fingerprint("Pose:f64 x,f64 y,f64 theta");
    const std::uint32_t b = schema_fingerprint("Pose:f64 x,f64 y,f64 theta");
    const std::uint32_t c = schema_fingerprint("Pose:f64 x,f64 y,f32 theta");
    RDB_CHECK_EQ(a, b);
    RDB_CHECK(a != c);
}
