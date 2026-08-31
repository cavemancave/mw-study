#include "rdb/transport/framing.h"

#include <cstddef>
#include <string>
#include <vector>

#include "rdb/testing/check.h"

using namespace rdb;

namespace {

std::vector<std::byte> bytes_of(const std::string& s) {
    std::vector<std::byte> out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return out;
}

std::string string_of(const std::vector<std::byte>& b) {
    std::string out;
    out.reserve(b.size());
    for (std::byte v : b) out.push_back(static_cast<char>(v));
    return out;
}

}  // namespace

RDB_TEST(framing_round_trip_single_frame) {
    const std::vector<std::byte> payload = bytes_of("hello middleware");
    std::vector<std::byte> wire;
    encode_frame(payload.data(), payload.size(), wire);
    RDB_CHECK_EQ(wire.size(), kFramePrefixSize + payload.size());

    FrameDecoder decoder;
    decoder.feed(wire.data(), wire.size());
    std::vector<std::byte> frame;
    RDB_CHECK(decoder.next(frame));
    RDB_CHECK_EQ(string_of(frame), std::string("hello middleware"));
    RDB_CHECK(!decoder.next(frame));
    RDB_CHECK(!decoder.error());
}

// TCP 每次 read 的边界和消息边界无关：逐字节喂也必须能正确重组。
RDB_TEST(framing_handles_byte_by_byte_delivery) {
    std::vector<std::byte> wire;
    for (int i = 0; i < 3; ++i) {
        const std::vector<std::byte> payload = bytes_of("frame-" + std::to_string(i));
        encode_frame(payload.data(), payload.size(), wire);
    }

    FrameDecoder decoder;
    std::vector<std::string> decoded;
    std::vector<std::byte> frame;
    for (std::size_t i = 0; i < wire.size(); ++i) {
        decoder.feed(wire.data() + i, 1);
        while (decoder.next(frame)) decoded.push_back(string_of(frame));
    }
    RDB_CHECK_EQ(decoded.size(), std::size_t{3});
    RDB_CHECK_EQ(decoded[0], std::string("frame-0"));
    RDB_CHECK_EQ(decoded[2], std::string("frame-2"));
    RDB_CHECK_EQ(decoder.frames_decoded(), std::uint64_t{3});
}

RDB_TEST(framing_handles_multiple_frames_in_one_read) {
    std::vector<std::byte> wire;
    for (int i = 0; i < 50; ++i) {
        const std::vector<std::byte> payload = bytes_of(std::string(static_cast<std::size_t>(i), 'x'));
        encode_frame(payload.data(), payload.size(), wire);
    }
    FrameDecoder decoder;
    decoder.feed(wire.data(), wire.size());

    int count = 0;
    std::vector<std::byte> frame;
    while (decoder.next(frame)) {
        RDB_CHECK_EQ(frame.size(), static_cast<std::size_t>(count));
        ++count;
    }
    RDB_CHECK_EQ(count, 50);
    RDB_CHECK_EQ(decoder.buffered(), std::size_t{0});
}

RDB_TEST(framing_empty_payload_is_valid) {
    std::vector<std::byte> wire;
    encode_frame(nullptr, 0, wire);
    FrameDecoder decoder;
    decoder.feed(wire.data(), wire.size());
    std::vector<std::byte> frame;
    RDB_CHECK(decoder.next(frame));
    RDB_CHECK(frame.empty());
}

// 长度前缀没有上限 = 让对端决定你分配多少内存，必须显式拒绝。
RDB_TEST(framing_rejects_oversized_length) {
    std::vector<std::byte> wire;
    const std::vector<std::byte> payload = bytes_of("0123456789");
    encode_frame(payload.data(), payload.size(), wire);

    FrameDecoder decoder(4);  // 最大 4 字节
    decoder.feed(wire.data(), wire.size());
    std::vector<std::byte> frame;
    RDB_CHECK(!decoder.next(frame));
    RDB_CHECK(decoder.error());
}

RDB_TEST(framing_detects_desync_by_magic) {
    std::vector<std::byte> wire = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                   std::byte{0x04}, std::byte{0x00}, std::byte{0x00},
                                   std::byte{0x00}, std::byte{0x00}};
    FrameDecoder decoder;
    decoder.feed(wire.data(), wire.size());
    std::vector<std::byte> frame;
    RDB_CHECK(!decoder.next(frame));
    RDB_CHECK(decoder.error());

    decoder.reset();
    RDB_CHECK(!decoder.error());
}
