// 字节流分帧。TCP 是字节流不是消息流：一次 read 可能拿到半条、一条半或十条消息。
// 帧格式：[magic u32][length u32][payload...]，magic 用来在出错时立刻发现流已错位。
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rdb {

inline constexpr std::uint32_t kFrameMagic = 0x314D5246u;  // "FRM1"
inline constexpr std::size_t kFramePrefixSize = 8;

inline void encode_frame(const std::byte* payload, std::size_t size, std::vector<std::byte>& out) {
    const std::size_t offset = out.size();
    out.resize(offset + kFramePrefixSize + size);
    std::byte* p = out.data() + offset;
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::byte>((kFrameMagic >> (8 * i)) & 0xFFu);
    const std::uint32_t len = static_cast<std::uint32_t>(size);
    for (int i = 0; i < 4; ++i) p[4 + i] = static_cast<std::byte>((len >> (8 * i)) & 0xFFu);
    if (size > 0) std::memcpy(p + kFramePrefixSize, payload, size);
}

inline std::vector<std::byte> encode_frame(const std::byte* payload, std::size_t size) {
    std::vector<std::byte> out;
    encode_frame(payload, size, out);
    return out;
}

class FrameDecoder {
public:
    // max_frame_bytes 是安全边界：没有上限的长度前缀等于让对端决定你分配多少内存。
    explicit FrameDecoder(std::size_t max_frame_bytes = 8u * 1024u * 1024u)
        : max_frame_bytes_(max_frame_bytes) {}

    void feed(const std::byte* data, std::size_t size) {
        if (error_) return;
        buffer_.insert(buffer_.end(), data, data + size);
    }

    // 取出一条完整帧；返回 false 表示数据还不够，或已进入错误状态。
    bool next(std::vector<std::byte>& out) {
        if (error_) return false;
        if (buffer_.size() - head_ < kFramePrefixSize) {
            compact();
            return false;
        }
        const std::byte* p = buffer_.data() + head_;
        std::uint32_t magic = 0;
        for (int i = 0; i < 4; ++i) magic |= static_cast<std::uint32_t>(p[i]) << (8 * i);
        if (magic != kFrameMagic) {
            error_ = true;  // 无法安全重同步：调用方应断开连接重连
            return false;
        }
        std::uint32_t len = 0;
        for (int i = 0; i < 4; ++i) len |= static_cast<std::uint32_t>(p[4 + i]) << (8 * i);
        if (len > max_frame_bytes_) {
            error_ = true;
            return false;
        }
        const std::size_t total = kFramePrefixSize + len;
        if (buffer_.size() - head_ < total) {
            compact();
            return false;
        }
        out.assign(p + kFramePrefixSize, p + total);
        head_ += total;
        ++frames_;
        compact();
        return true;
    }

    bool error() const { return error_; }
    std::size_t buffered() const { return buffer_.size() - head_; }
    std::uint64_t frames_decoded() const { return frames_; }

    void reset() {
        buffer_.clear();
        head_ = 0;
        error_ = false;
    }

private:
    // 已消费部分超过一半时才搬移，避免每帧都 O(n) 拷贝。
    void compact() {
        if (head_ == 0) return;
        if (head_ == buffer_.size()) {
            buffer_.clear();
            head_ = 0;
            return;
        }
        if (head_ * 2 >= buffer_.size()) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(head_));
            head_ = 0;
        }
    }

    std::vector<std::byte> buffer_;
    std::size_t head_ = 0;
    std::size_t max_frame_bytes_;
    std::uint64_t frames_ = 0;
    bool error_ = false;
};

}  // namespace rdb
