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

static int test_unique_type_and_name() {
  for (std::size_t i = 0; i < kProtocolDescriptors.size(); ++i) {
    for (std::size_t j = i + 1; j < kProtocolDescriptors.size(); ++j) {
      CHECK(kProtocolDescriptors[i].type != kProtocolDescriptors[j].type);
      CHECK(kProtocolDescriptors[i].name != kProtocolDescriptors[j].name);
    }
  }
  CHECK(kProtocolDescriptors.size() == 54);  // 设计合同 §8 分配总数
  return 0;
}

static int test_find_protocol()
{
  for (const auto& descriptor : kProtocolDescriptors) {
    CHECK(find_protocol(descriptor.type) == &descriptor);
  }

  CHECK(find_protocol(static_cast<ProtocolType>(0x03)) == nullptr);
  return 0;
}

static int test_request_response_pairing() {
  for (const auto& d : kProtocolDescriptors) {
    if (d.direction == Direction::request) {
      CHECK(d.response.has_value());
      const auto* response = find_protocol(*d.response);
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

constexpr bool is_preauth_type(const ProtocolType type) noexcept
{
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

static int test_states()
{
  CHECK(has_state(ConnectionStates::awaiting_hello,
    ConnectionState::awaiting_hello));
  CHECK(!has_state(ConnectionStates::awaiting_hello,
      ConnectionState::unauthenticated));

  CHECK(has_state(ConnectionStates::unauthenticated,
      ConnectionState::unauthenticated));
  CHECK(!has_state(ConnectionStates::unauthenticated,
      ConnectionState::authenticated));

  CHECK(has_state(ConnectionStates::authenticated,
      ConnectionState::authenticated));
  CHECK(!has_state(ConnectionStates::authenticated,
      ConnectionState::awaiting_hello));

  // D72 逐类精确断言：相等而非“包含”，防止状态集合被错误扩大。
  CHECK(find_protocol(ProtocolType::hello_request)->allowed_states
      == ConnectionStates::awaiting_hello);
  CHECK(find_protocol(ProtocolType::hello_response)->allowed_states
      == ConnectionStates::awaiting_hello);
  CHECK(find_protocol(ProtocolType::protocol_error)->allowed_states
      == ConnectionStates::all);
  CHECK(find_protocol(ProtocolType::heartbeat)->allowed_states
      == (ConnectionStates::unauthenticated | ConnectionStates::authenticated));


  for (const auto& d : kProtocolDescriptors) {
    if (is_preauth_type(d.type)) {
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

static int test_limits_and_timeouts() {
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
  CHECK(find_protocol(ProtocolType::hello_request)->default_timeout_ms == 5000);
  CHECK(find_protocol(ProtocolType::login_request)->default_timeout_ms ==
        10000);
  CHECK(find_protocol(ProtocolType::logout_request)->default_timeout_ms ==
        2000);
  CHECK(find_protocol(ProtocolType::conversation_history_request)
            ->default_timeout_ms == 10000);
  return 0;
}

static int test_capability() {
  CHECK(find_protocol(ProtocolType::message_send_request)->capability ==
        "text_v1");
  CHECK(find_protocol(ProtocolType::message_received_push)->capability ==
        "text_v1");
  CHECK(find_protocol(ProtocolType::hello_request)->capability.empty());
  CHECK(find_protocol(ProtocolType::login_request)->capability.empty());
  return 0;
}

int main() {
  if (const int rc = test_unique_type_and_name()) {
    return rc;
  }
  if (const int rc = test_request_response_pairing()) {
    return rc;
  }
  if (const int rc = test_find_protocol()) {
    return rc;
  }
  if (const int rc = test_states()) {
    return rc;
  }
  if (const int rc = test_limits_and_timeouts()) {
    return rc;
  }
  if (const int rc = test_capability()) {
    return rc;
  }
  return 0;
}