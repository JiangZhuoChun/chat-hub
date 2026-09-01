#pragma once

// 8 字节帧头与流式分帧（D07—D09、D70、D11）。
// 接收顺序固定：收齐帧头 → 校验 magic/版本/type/正文长度 → 分配前拒绝超限
// → 收齐正文 → UTF-8 校验 → 产出完整帧（D08；详细设计 §2 接收顺序）。
// magic/帧头/长度错误直接进入 failed 态，不扫描重同步（D70）；
// 可识别的版本不兼容由 Session 层发送 protocol_error 后关闭（M1-6/M1-7）。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "protocol_descriptor.hpp"

namespace chathub::contracts {

inline constexpr std::uint8_t kFrameMagicHigh = 0x43;  // 'C'（D70）
inline constexpr std::uint8_t kFrameMagicLow = 0x48;   // 'H'（D70）
inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kFrameHeaderSize = 8;  // D08

// 合法 UTF-8（拒绝代理区、超长编码与 > U+10FFFF）。
inline bool is_valid_utf8(std::string_view text) noexcept
{
  const auto* data =
      reinterpret_cast<const unsigned char*>(text.data());

  const auto size = text.size();
  std::size_t i = 0;

  while (i < size)
  {
    const auto c = data[i];

    if (c <= 0x7F)
    {
      ++i;
      continue;
    }

    std::size_t length = 0;

    if (c >= 0xC2 && c <= 0xDF)
      length = 2;
    else if (c >= 0xE0 && c <= 0xEF)
      length = 3;
    else if (c >= 0xF0 && c <= 0xF4)
      length = 4;
    else
      return false;

    if (i + length > size)
      return false;

    //检查UTF-8 多字节字符的后续字节，是否都符合 10xxxxxx 的格式
    for (std::size_t j = 1; j < length; ++j)
    {
      if ((data[i + j] & 0xC0) != 0x80)
        return false;
    }

    const auto second = data[i + 1];

    if (c == 0xE0 && second < 0xA0)
      return false;// 超长编码
    if (c == 0xED && second > 0x9F)
      return false;// UTF-16 surrogate
    if (c == 0xF0 && second < 0x90)
      return false;// 超长编码
    if (c == 0xF4 && second > 0x8F)
      return false;// > U+10FFFF

    i += length;
  }
  return true;
}

struct Frame {
  ProtocolType type = ProtocolType::protocol_error;
  std::string body;     // 已通过 UTF-8 校验的正文；空正文合法（heartbeat）
};

enum class FrameErrorKind {
    decoder_failed,   // 初始值：解码器已进入失败态，必须丢弃并关闭连接（D11）
    bad_magic,        // D70：直接关闭，不扫描重同步
    bad_version,      // D70：可识别的版本不兼容
    unknown_type,     // D11：协议错误（含预留 type）
    body_too_large,   // D09：分配前拒绝
    invalid_utf8,     // D11：正文非合法 UTF-8
};

class FrameDecoder {
public:
  // 喂入任意长度字节：半包、粘包统一处理。失败后保持 failed，不再产出
  void feed(const std::uint8_t* date,std::size_t size) {
    if (failed_) {
      return;
    }
    buffer_.insert(buffer_.end(),date,date + size);
    process();
    compact();
  }

  bool next(Frame& out) {
    if (failed_ || ready_.empty()) {
      return false;
    }
    out = std::move(ready_.front());
    ready_.erase(ready_.begin());
    return true;
  }

  [[nodiscard]] bool failed() const noexcept{return failed_;}
  [[nodiscard]] FrameErrorKind error() const noexcept{return error_;}

private:
  void fail(FrameErrorKind kind) {
    failed_ = true;
    error_ = kind;
  }

  // 处理 buffer_ 中已收齐的帧
  void process() {
    while (!failed_) {
      if (!header_done_) {
        if (buffer_.size() - consumed_ < kFrameHeaderSize) {
          return;  // 半包：帧头未收齐
        }
        parse_header(consumed_);
        if (failed_) {
          return;
        }
        header_done_ = true;
      }

      if (buffer_.size() - consumed_ < kFrameHeaderSize + body_length_) {
        return;  // 半包：正文未收齐
      }
      // 分配前已完成 max_body 校验，此处 body_length ≤ 65536（D09）。
      std::string body(reinterpret_cast<const char*>(
        buffer_.data() + consumed_ + kFrameHeaderSize),body_length_);
      header_done_ = false;

      if (!is_valid_utf8(body)) {
        fail(FrameErrorKind::invalid_utf8);
        return;
      }
      ready_.push_back(Frame{frame_type_,std::move(body)});
    }
  }

  void parse_header(std::size_t offset) {
    if (buffer_[offset] != kFrameMagicHigh || buffer_[offset + 1] != kFrameMagicLow) {
      fail(FrameErrorKind::bad_magic);
      return;
    }
    if (buffer_[offset + 2] != kProtocolVersion) {
      fail(FrameErrorKind::bad_version);
      return;
    }

    const auto* descriptor =
      find_protocol(static_cast<ProtocolType>(buffer_[offset + 3]));
    if (descriptor == nullptr) {
      fail(FrameErrorKind::unknown_type);
      return;
    }

    const std::uint32_t body_length =
      (static_cast<std::uint32_t>(buffer_[offset + 4] << 24)) |
      (static_cast<std::uint32_t>(buffer_[offset + 5] << 16)) |
      (static_cast<std::uint32_t>(buffer_[offset + 6] << 8))  |
      (static_cast<std::uint32_t>(buffer_[offset + 7]));
    if (body_length > descriptor->max_body) {
      fail(FrameErrorKind::body_too_large); // D09：分配前拒绝
      return;
    }

    frame_type_ = descriptor->type;
    body_length_ = body_length;
  }

  // 丢弃已消费前缀，防止长连接下 buffer_ 无界增长。
  void compact() {
    if (consumed_ > 0) {
      buffer_.erase(buffer_.begin(),
        buffer_.begin() + static_cast<std::ptrdiff_t>(consumed_));
      consumed_ = 0;
    }
  }
  std::vector<std::uint8_t> buffer_;
  std::size_t consumed_ = 0;
  std::size_t body_length_ = 0;
  ProtocolType frame_type_ = ProtocolType::protocol_error;
  bool header_done_ = false;
  bool failed_ = false;
  FrameErrorKind error_ = FrameErrorKind::decoder_failed;
  std::vector<Frame> ready_;
};

// 编码一帧：8 字节帧头（大端）＋正文；正文超过该 type 上限时双向拒绝（D09）。
inline  std::optional<std::vector<std::uint8_t>> encode_frame(ProtocolType type,std::string_view body) {
  const auto* descriptor = find_protocol(type);
  if (descriptor == nullptr || body.size() > descriptor->max_body) {
    return std::nullopt;
  }

  std::vector<std::uint8_t> out;
  out.reserve(kFrameHeaderSize + body.size());
  out.push_back(kFrameMagicHigh);
  out.push_back(kFrameMagicLow);
  out.push_back(kProtocolVersion);

  out.push_back(static_cast<std::uint8_t>(type));
  out.push_back(static_cast<std::uint8_t>((body.size() >> 24) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((body.size() >> 16) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((body.size() >> 8) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>(body.size() & 0xFFu));
  out.insert(out.end(),body.begin(),body.end());

  return out;
}
}  // namespace chathub::contracts