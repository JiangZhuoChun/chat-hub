// M1-3 注册表测试（D218）：type/name
// 唯一、请求响应配对、状态合法、上限与超时、capability。

#include <cstdio>
#include <cstddef>

#include "chathub/contracts/protocol_descriptor.hpp"

using namespace chathub::contracts;

#define CHECK(expr)                                    \
  do {                                                 \
    if (!(expr)) {                                     \
      std::fputs("CHECK failed: " #expr "\n", stderr); \
      return 1;                                        \
    }                                                  \
  } while (false)

static int testUniqueTypeAndName() {
  for (std::size_t i = 0; i < kProtocolDescriptors.size(); ++i) {
    for (std::size_t j = i + 1; j < kProtocolDescriptors.size(); ++j) {
      CHECK(kProtocolDescriptors[i].type != kProtocolDescriptors[j].type);
      CHECK(kProtocolDescriptors[i].name != kProtocolDescriptors[j].name);
    }
  }
  CHECK(kProtocolDescriptors.size() == 54);  // 设计合同 §8 分配总数
  return 0;
}

static int testFindProtocol()
{
  // 已登记 type 必须定位到自身；不仅验证“找到了”，还验证没有错指到别的描述项。
  for (const auto& descriptor : kProtocolDescriptors) {
    CHECK(findProtocol(descriptor.type) == &descriptor);
  }

  // 0x03 是设计合同保留号，供 M1-4 将其判为未知 type，不能被意外注册。
  CHECK(findProtocol(static_cast<ProtocolType>(0x03)) == nullptr);
  return 0;
}

static int testRequestResponsePairing() {
  for (const auto& d : kProtocolDescriptors) {
    if (d.direction == Direction::request) {
      CHECK(d.response.has_value());
      const auto* response = findProtocol(*d.response);
      CHECK(response != nullptr);
      CHECK(response->direction == Direction::response);
    } else if (d.direction == Direction::response) {
      CHECK(!d.response.has_value());
      unsigned owners = 0;  // 每个响应恰好被一个请求引用
      for (const auto& other : kProtocolDescriptors) {
        if (other.response == d.type) {
          ++owners;
        }
      }
      CHECK(owners == 1);
    } else {  // push／bidirectional 无配对响应
      CHECK(!d.response.has_value());
    }
  }
  return 0;
}

constexpr bool isPreauthType(const ProtocolType type) noexcept
{
  // 这十个 type 在 hello 后、认证成功前使用；D72 要求它们仅允许未认证状态。
  switch (type) {
    case ProtocolType::register_prepare_request:
    case ProtocolType::register_prepare_response:
    case ProtocolType::register_finalize_request:
    case ProtocolType::register_finalize_response:
    case ProtocolType::login_request:
    case ProtocolType::login_response:
    case ProtocolType::session_resume_request:
    case ProtocolType::session_resume_response:
    case ProtocolType::password_reset_request:
    case ProtocolType::password_reset_response:
      return true;
    default:
      return false;
  }
}

static int testStates()
{
  CHECK(hasState(ConnectionStates::awaiting_hello,
    ConnectionState::awaiting_hello));
  CHECK(!hasState(ConnectionStates::awaiting_hello,
      ConnectionState::unauthenticated));

  CHECK(hasState(ConnectionStates::unauthenticated,
      ConnectionState::unauthenticated));
  CHECK(!hasState(ConnectionStates::unauthenticated,
      ConnectionState::authenticated));

  // authenticated 对应第三个状态位（1 << 2）；正反例防止位掩码再次错位。
  CHECK(hasState(ConnectionStates::authenticated,
      ConnectionState::authenticated));
  CHECK(!hasState(ConnectionStates::authenticated,
      ConnectionState::awaiting_hello));

  // D72 逐类精确断言：相等而非“包含”，防止状态集合被错误扩大。
  CHECK(findProtocol(ProtocolType::hello_request)->allowed_states
      == ConnectionStates::awaiting_hello);
  CHECK(findProtocol(ProtocolType::hello_response)->allowed_states
      == ConnectionStates::awaiting_hello);
  CHECK(findProtocol(ProtocolType::protocol_error)->allowed_states
      == ConnectionStates::all);
  CHECK(findProtocol(ProtocolType::heartbeat)->allowed_states
      == (ConnectionStates::unauthenticated | ConnectionStates::authenticated));


  for (const auto& d : kProtocolDescriptors) {
    if (isPreauthType(d.type)) {
      CHECK(d.allowed_states == ConnectionStates::unauthenticated);
    } else if (d.type == ProtocolType::hello_request
        || d.type == ProtocolType::hello_response
        || d.type == ProtocolType::protocol_error
        || d.type == ProtocolType::heartbeat) {
      continue;  // 上面已精确断言
        } else {
          CHECK(d.allowed_states == ConnectionStates::authenticated);
        }
    CHECK(d.allowed_states != ConnectionStates::none);
  }
  return 0;
}

static int testLimitsAndTimeouts() {
  for (const auto& d : kProtocolDescriptors) {
    CHECK(d.max_body <= kMaxJsonBody);  // D09
    if (d.type == ProtocolType::heartbeat) {
      CHECK(d.max_body == 0);  // 空正文（D72）
    }
    if (d.direction == Direction::request) {
      CHECK(d.default_timeout_ms > 0);  // D74
    } else {
      CHECK(d.default_timeout_ms == 0);
    }
  }
  CHECK(findProtocol(ProtocolType::hello_request)->default_timeout_ms == 5000);
  CHECK(findProtocol(ProtocolType::login_request)->default_timeout_ms ==
        10000);
  CHECK(findProtocol(ProtocolType::logout_request)->default_timeout_ms ==
        2000);
  CHECK(findProtocol(ProtocolType::conversation_history_request)
            ->default_timeout_ms == 10000);
  return 0;
}

static int testCapability() {
  CHECK(findProtocol(ProtocolType::message_send_request)->capability ==
        "text_v1");
  CHECK(findProtocol(ProtocolType::message_received_push)->capability ==
        "text_v1");
  CHECK(findProtocol(ProtocolType::hello_request)->capability.empty());
  CHECK(findProtocol(ProtocolType::login_request)->capability.empty());
  return 0;
}

int main() {
  if (const int rc = testUniqueTypeAndName()) {
    return rc;
  }
  if (const int rc = testRequestResponsePairing()) {
    return rc;
  }
  if (const int rc = testFindProtocol()) {
    return rc;
  }
  if (const int rc = testStates()) {
    return rc;
  }
  if (const int rc = testLimitsAndTimeouts()) {
    return rc;
  }
  if (const int rc = testCapability()) {
    return rc;
  }
  return 0;
}