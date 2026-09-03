#pragma once

// 8 字节帧头与流式分帧（D07—D09、D70、D11）。
// 接收顺序固定：收齐帧头 → 校验 magic/版本/type/正文长度 → 分配前拒绝超限
// → 收齐正文 → UTF-8 校验 → 产出完整帧（D08；详细设计 §2 接收顺序）。
// magic/帧头/长度错误直接进入 failed 态，不扫描重同步（D70）；
// 可识别的版本不兼容由 Session 层发送 protocol_error 后关闭（M1-6/M1-7）。

// 0      1      2        3        4         7
// +------+------+--------+--------+-----------+
// | 'C'  | 'H'  | ver=1  | type   | body_len  |  大端 uint32
// +------+------+--------+--------+-----------+
// |              UTF-8 正文（可为空）          |
// +-------------------------------------------+

#include <cstddef>
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
inline bool isValidUtf8(std::string_view text) noexcept
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
  void feedBytes(const std::uint8_t* data,std::size_t size) {
    if (failed_) {
      return;
    }
    input_buffer_.insert(input_buffer_.end(),data,data + size);
    // 处理 buffer_ 中已收齐的帧
    decodeAvailableFrames();
    // 丢弃已消费前缀，防止长连接下 buffer_ 无界增长。
    discardConsumedBytes();
  }

  //取出一帧；失败后不再产出
  bool tryPopFrame(Frame& out) {
    if (failed_ || decoded_frames_.empty()) {
      return false;
    }
    out = std::move(decoded_frames_.front());
    decoded_frames_.erase(decoded_frames_.begin());
    return true;
  }

  [[nodiscard]] bool hasFailed() const noexcept{return failed_;}
  [[nodiscard]] FrameErrorKind error() const noexcept{return error_;}

private:
  void setFailure(FrameErrorKind kind) {
    failed_ = true;
    error_ = kind;
  }

  // 处理 buffer_ 中已收齐的帧
  void decodeAvailableFrames() {
    while (true) {
      if (!has_pending_header_) {
        if (input_buffer_.size() - consumed_prefix_size_ < kFrameHeaderSize) {
          return;  // 帧头半包，保留缓冲等待下次 feedBytes
        }

        if (!parseAndValidateHeader(consumed_prefix_size_)) {
          return;
        }
        has_pending_header_ = true;
      }

      //帧头已经收齐并校验过
      if (input_buffer_.size() - consumed_prefix_size_ < kFrameHeaderSize + body_length_) {
        return;  // 正文半包：正文未收齐
      }

      // 分配前已完成 max_body 校验，此处 body_length ≤ 65536（D09）。
      std::string body(reinterpret_cast<const char*>(
        input_buffer_.data() + consumed_prefix_size_ + kFrameHeaderSize),body_length_);
      has_pending_header_ = false;

      if (!isValidUtf8(body)) {
        setFailure(FrameErrorKind::invalid_utf8);
        return;
      }
      decoded_frames_.push_back(Frame{frame_type_,std::move(body)});
      // 当前帧已完整转移到 ready_；推进游标后，循环才能从下一帧开始解析。
      consumed_prefix_size_ += kFrameHeaderSize + body_length_;
    }
  }

  //校验 magic/version/type，分配前用 max_body 卡长度
  bool parseAndValidateHeader(std::size_t offset) {
    if (input_buffer_[offset] != kFrameMagicHigh || input_buffer_[offset + 1] != kFrameMagicLow) {
      setFailure(FrameErrorKind::bad_magic);
      return false;
    }
    if (input_buffer_[offset + 2] != kProtocolVersion) {
      setFailure(FrameErrorKind::bad_version);
      return false;
    }

    const auto* descriptor =
      findProtocol(static_cast<ProtocolType>(input_buffer_[offset + 3]));
    if (descriptor == nullptr) {
      setFailure(FrameErrorKind::unknown_type);
      return false;
    }

    // 必须在左移前提升到 32 位，避免 uint8_t 提升为 int 后移位溢出。
    const std::uint32_t body_length =
      (static_cast<std::uint32_t>(input_buffer_[offset + 4]) << 24) |
      (static_cast<std::uint32_t>(input_buffer_[offset + 5]) << 16) |
      (static_cast<std::uint32_t>(input_buffer_[offset + 6]) << 8)  |
      static_cast<std::uint32_t>(input_buffer_[offset + 7]);
    if (body_length > descriptor->max_body) {
      setFailure(FrameErrorKind::body_too_large); // D09：分配前拒绝
      return false;
    }

    frame_type_ = descriptor->type;
    body_length_ = body_length;
    return true;
  }

  // 丢弃已消费前缀，防止长连接下 buffer_ 无界增长。
  void discardConsumedBytes() {
    if (consumed_prefix_size_ > 0) {
      input_buffer_.erase(input_buffer_.begin(),
        input_buffer_.begin() + static_cast<std::ptrdiff_t>(consumed_prefix_size_));
      consumed_prefix_size_ = 0;
    }
  }
  std::vector<std::uint8_t> input_buffer_;  //尚待解码的输入缓冲
  std::size_t consumed_prefix_size_ = 0;  //本轮已消费、待丢弃的前缀长度
  std::size_t body_length_ = 0;
  ProtocolType frame_type_ = ProtocolType::protocol_error;
  bool has_pending_header_ = false; //帧头已校验，等待正文
  bool failed_ = false;
  FrameErrorKind error_ = FrameErrorKind::decoder_failed;
  std::vector<Frame> decoded_frames_; //已完成、等待调用方取走的帧
};

// 编码一帧：8 字节帧头（大端）＋正文；正文超过该 type 上限时双向拒绝（D09）。
inline  std::optional<std::vector<std::uint8_t>> encodeFrame(ProtocolType type,std::string_view body) {
  const auto* descriptor = findProtocol(type);
  // 出站也只允许已登记 type、长度合规且 UTF-8 合法的正文，与入站校验对称。
  if (descriptor == nullptr || body.size() > descriptor->max_body ||
      !isValidUtf8(body)) {
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