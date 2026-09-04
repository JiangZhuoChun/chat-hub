#pragma once

#include <charconv>      // std::from_chars
#include <cstddef>       // std::size_t
#include <cstdint>       // std::uint64_t
#include <optional>      // std::optional
#include <string>        // std::string、std::to_string
#include <string_view>   // std::string_view
#include <system_error>  // std::errc
#include <type_traits>   // std::is_convertible_v
#include <utility>       // std::move

// 强类型身份与序号（D209）：不同身份和序号禁止隐式互转；
// 构造／解析时验证格式（D44、路线 9.4），失败返回空
// optional，不抛异常（D211）。
// 这些对象只保存已经校验、规范化后的值，避免调用者在后续流程重复校验。

namespace chathub::contracts {

namespace detail {
// detail 中的函数只服务于本头文件的解析，不是对外业务 API。
// 4—32 个英文字母或数字，必须以字母开头（D44）
inline bool isAccountName(std::string_view name) noexcept {
  if (name.size() < 4 || name.size() > 32) {
    return false;
  }
  const auto letter = [](char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  };
  const auto alnum = [letter](char c) noexcept {
    return letter(c) || (c >= '0' && c <= '9');
  };

  if (!letter(name.front())) {
    return false;
  }
  for (const char c : name.substr(1)) {
    if (!alnum(c)) return false;
  }
  return true;
}

// 36 字符 8-4-4-4-12 十六进制；线格式小写（D69），版本位 = v4，变体位 =8/9/a/b。
inline bool isUuid(std::string_view id) noexcept {
  if (id.size() != 36) {
    return false;
  }
  for (std::size_t i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (id[i] != '-') {
        return false;
      }
      continue;
    }
    // D69：request_id 线格式为小写，仅接受 0-9 与 a-f（拒绝大写）。
    const bool hex =
        (id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f');
    if (!hex) {
      return false;
    }
  }
  // 版本位（第 15 字符，即 index 14）必须是 4（UUID v4，D69）。
  if (id[14] != '4') {
    return false;
  }
  // 变体位（第 20 字符，即 index 19）按 RFC 4122 限制为 8/9/a/b。
  if (id[19] != '8' && id[19] != '9' && id[19] != 'a' && id[19] != 'b') {
    return false;
  }
  return true;
}

inline std::string toLowerAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  return out;
}
}  // namespace detail

class AccountName {
 public:
  // 成功时把值统一存成小写；因此 value() 可直接用于唯一性比较和登录查找。
  static std::optional<AccountName> parse(std::string_view name) {
    if (!detail::isAccountName(name)) {
      return std::nullopt;
    }
    return AccountName{detail::toLowerAscii(name)};
  }

  [[nodiscard]] const std::string& value() const noexcept { return value_; }

 private:
  explicit AccountName(std::string value) : value_(std::move(value)) {}

  std::string value_;
};

template <typename Tag>
class UuidId {
 public:
  // Tag 只存在于类型系统：三个 UUID 的运行时表示相同，但不能互相传参。
  // 解析要求小写 UUID v4（D69），失败返回空 optional
  static std::optional<UuidId> parse(std::string_view id) {
    if (!detail::isUuid(id)) {
      return std::nullopt;
    }
    return UuidId{detail::toLowerAscii(id)};
  }

  UuidId() = default;

  [[nodiscard]] const std::string& value() const noexcept { return value_; }

 private:
  explicit UuidId(std::string value) : value_(std::move(value)) {}

  std::string value_;
};

// 空 Tag 不携带数据，只为 UuidId 生成彼此不同的 C++ 类型。
struct MessageIdTag {};
struct LocalMessageIdTag {};
struct RequestIdTag {};

using MessageId = UuidId<MessageIdTag>;
using LocalMessageId = UuidId<LocalMessageIdTag>;
using RequestId = UuidId<RequestIdTag>;

template <typename Tag>
class DecimalSeq {
 public:
  // 线上使用规范十进制字符串（路线 9.4）：非空、全数字、无前导零、不溢出。
  // from_chars 不分配内存；result.ptr 必须走到末尾，才能拒绝 "4x"这类部分解析。
  static std::optional<DecimalSeq> parse(std::string_view text) {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) {
      return std::nullopt;
    }
    std::uint64_t parsed = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
      return std::nullopt;
    }
    return DecimalSeq{parsed};
  }

  static DecimalSeq of(std::uint64_t value) noexcept {
    return DecimalSeq{value};
  }

  [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

  [[nodiscard]] std::string toDecimalString() const {
    return std::to_string(value_);
  }

 private:
  explicit DecimalSeq(std::uint64_t value) noexcept : value_(value) {}

  std::uint64_t value_;
};

struct DeliverySeqTag {};
struct ConversationSeqTag {};

using DeliverySeq = DecimalSeq<DeliverySeqTag>;
using ConversationSeq = DecimalSeq<ConversationSeqTag>;

// 编译期合同：若未来修改让不同身份或序号可隐式转换，构建立即失败。
static_assert(!std::is_convertible_v<MessageId, LocalMessageId>);
static_assert(!std::is_convertible_v<LocalMessageId, MessageId>);
static_assert(!std::is_convertible_v<RequestId, MessageId>);
static_assert(!std::is_convertible_v<DeliverySeq, ConversationSeq>);
static_assert(!std::is_convertible_v<MessageId, std::string>);

}  // namespace chathub::contracts
