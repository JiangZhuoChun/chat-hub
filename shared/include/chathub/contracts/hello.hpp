#pragma once

/*
hello 帧的线协议值类型与 capability 协商（D72、D197、D198）：纯 C++20，
capability 一般翻译成“能力”“功能能力”或者“支持项”
理解成：客户端或服务端声明“我支持哪些协议功能”。
不依赖 Qt / Asio / JSON / 文件系统 / 平台 API。客户端用 Qt JSON、服务端用
Boost.JSON 在各自适配层做 wire ↔ 值对象映射（D66、D198）
*/

#include <cstdint>
#include <set>
#include <string>
#include <string_view>

namespace chathub::contracts {

// capability 名称（D197）：稳定字符串，在线以 JSON 字符串数组元素传输。
// 首版只启用 text_v1；file_v1 / voice_v1 为未来能力，本步仅保留常量与交集
// 机制，不实现其内容（D207）。
inline constexpr std::string_view kCapabilityTextV1 = "text_v1";

// 版本字面量（D72）；首版服务端只支持客户端版本 "1.0"。
inline constexpr std::string_view kServerVersion = "1.0";
inline constexpr std::string_view kSupportClientVersion = "1.0";

// 平台字面量（D72）：首版仅 Windows 桌面。
inline constexpr std::string_view kPlatform = "windows";

// 服务端在 hello_response 下发的连接参数（D72、D73）
inline constexpr std::uint32_t kMaxJsonBytes = 65536;       // max_json：JSON 帧上限（D09）
inline constexpr std::uint32_t kMaxTextBytes = 4096;    // max_text：单条文字字节上限（D10）
inline constexpr std::uint32_t kHeartbeatIdleMs= 20000;     // 空闲 20 秒心跳（D73）
inline constexpr std::uint32_t kSessionTimeoutMs = 60000;   // 60 秒无有效帧断开（D55/D73

// 外部输入字段边界（D66 处理顺序 + 本步确认）：全帧 64 KiB 之外逐字段设上限，
// 双端 codec 在 wire 层校验；未知 capability 不在此拒绝（交集裁掉，D197）。
inline constexpr std::size_t kMaxClientVersionBytes = 32;
inline constexpr std::size_t kMaxPlatformBytes = 16;
inline constexpr std::size_t kMaxCapabilityValueBytes = 64;
inline constexpr std::size_t kMaxCapabilityCount = 16;

// “能力名称集合”的封装类：有序去重，支持求交集。元素为稳定 capability 字符串。
class CapabilitySet {
public:
  CapabilitySet() = default;

  CapabilitySet(std::initializer_list<std::string_view> values) {
    for (const auto value : values) {
      value_.emplace(value);
    }
  }

  [[nodiscard]] bool contains(std::string_view value) const {
    return value_.contains(std::string(value));
  }

  void insert(std::string_view value) { value_.emplace(value); }

  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

  [[nodiscard]] std::size_t size() const noexcept { return value_.size(); }

  [[nodiscard]] const std::set<std::string>& value() const noexcept {
    return value_;
  }

  // 求交集；b 为全表/空表均正确收敛（D197：服务端返回双方集合的交集）
  static CapabilitySet intersect(const CapabilitySet& a, const CapabilitySet& b) {
    CapabilitySet out;
    for (const auto& value : a.value_) {
      if (b.value_.contains(value)) {
        out.value_.insert(value);
      }
    }
    return out;
  }

private:
  std::set<std::string> value_;
};

// hello 请求字段（D72 + D197；D68 信封的 data 部分）。
struct HelloRequest {
  std::string client_version;
  std::string platform;         // 固定 "windows"
  CapabilitySet capabilities;   // 客户端声明能力
};
// hello 响应字段（D72 + D197；D68 信封的 data 部分）。
struct HelloResponse {
  std::string server_version = std::string(kServerVersion);
  std::int64_t server_time = 0;
  std::uint32_t max_json = kMaxJsonBytes;
  std::uint32_t max_text = kMaxTextBytes;
  std::uint32_t heartbeat_idle = kHeartbeatIdleMs;
  std::uint32_t timeout = kSessionTimeoutMs;
  CapabilitySet capabilities;     // 服务端 ∩ 客户端 的已协商能力
};
}