#pragma once

// 只读 ProtocolDescriptor 注册表（D218）：静态编译数据，逐字覆盖设计合同 §8
// “当前已分配的类型”（0x01—0x87，54 个；未列出范围保持预留）。
// 超时取自 D74；连接状态对齐 D72 状态机；capability 对齐 D197。

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace chathub::contracts {

enum class ProtocolType : std::uint8_t {
  protocol_error = 0x01,  // 服务端报告可识别的协议错误后关闭连接。
  heartbeat = 0x02,       // 双向空正文保活，不建立请求／响应配对。

  hello_request = 0x04,   // TLS 后的首个客户端请求，用于版本与能力协商。
  hello_response = 0x05,  // hello 的服务端响应，给出协商后的连接参数。

  register_prepare_request = 0x10,   // 创建待完成注册并请求后续确认材料。
  register_prepare_response = 0x11,  // 返回注册 prepare 阶段的结果。

  register_finalize_request = 0x12,   // 提交确认信息并完成账号激活。
  register_finalize_response = 0x13,  // 返回注册 finalize 阶段的结果。

  login_request = 0x14,   // 使用账号凭据建立新的认证会话。
  login_response = 0x15,  // 返回登录成功或受控业务失败。

  session_resume_request = 0x16,   // 用已有会话材料恢复认证状态。
  session_resume_response = 0x17,  // 返回会话恢复结果。

  password_reset_request = 0x18,   // 发起或提交密码重置流程。
  password_reset_response = 0x19,  // 返回密码重置流程的结果。

  logout_request = 0x1A,   // 主动撤销当前已认证会话。
  logout_response = 0x1B,  // 确认登出处理已完成。

  profile_update_request = 0x1C,   // 修改当前账号的公开资料。
  profile_update_response = 0x1D,  // 返回资料更新结果。

  user_lookup_request = 0x30,   // 按受限条件查询其他用户资料。
  user_lookup_response = 0x31,  // 返回用户查找结果。

  friend_request_create_request = 0x32,   // 向目标账号创建好友申请。
  friend_request_create_response = 0x33,  // 返回创建好友申请的结果。
  friend_request_list_request = 0x34,     // 分页读取当前账号的好友申请。
  friend_request_list_response = 0x35,    // 返回好友申请列表。
  friend_request_accept_request = 0x36,   // 接受一条待处理好友申请。
  friend_request_accept_response = 0x37,  // 返回接受申请后的关系结果。
  friend_request_reject_request = 0x38,   // 拒绝一条待处理好友申请。
  friend_request_reject_response = 0x39,  // 返回拒绝申请的结果。
  friend_request_withdraw_request = 0x3A,   // 撤回由当前账号发出的好友申请。
  friend_request_withdraw_response = 0x3B,  // 返回撤回申请的结果。
  friend_delete_request = 0x3C,   // 解除当前账号与目标账号的好友关系。
  friend_delete_response = 0x3D,  // 返回解除好友关系的结果。

  contact_list_request = 0x3E,   // 分页读取当前账号的联系人投影。
  contact_list_response = 0x3F,  // 返回联系人列表。

  message_send_request = 0x50,   // 向已建立关系的目标发送 text_v1 消息。
  message_send_response = 0x51,  // 返回消息落库与序号分配结果。

  delivery_progress_request = 0x52,   // 上报已连续处理完成的 delivery 序号。
  delivery_progress_response = 0x53,  // 确认服务端已持久化处理进度。

  conversation_list_request = 0x54,   // 分页读取会话列表投影。
  conversation_list_response = 0x55,  // 返回会话列表及游标。
  conversation_history_request = 0x56,   // 按会话与游标读取历史项目。
  conversation_history_response = 0x57,  // 返回升序展示的会话历史。
  conversation_read_request = 0x58,   // 上报当前会话的单调阅读进度。
  conversation_read_response = 0x59,  // 确认阅读进度已处理。
  conversation_clear_request = 0x5A,   // 清空当前账号视角下的会话内容。
  conversation_clear_response = 0x5B,  // 返回清空会话的结果。

  session_terminated_push = 0x80,   // 服务端通知当前会话已被终止。
  profile_updated_push = 0x81,      // 服务端推送联系人资料已变更。
  friend_request_received_push = 0x82,  // 服务端推送收到新的好友申请。
  friendship_changed_push = 0x83,       // 服务端推送好友关系状态变化。
  friend_request_changed_push = 0x84,   // 服务端推送既有好友申请状态变化。
  conversation_event_push = 0x85,       // 服务端推送会话中的系统事件。
  message_received_push = 0x86,         // 服务端推送接收方的 text_v1 消息。
  delivery_sync_complete_push = 0x87,   // 服务端确认补收窗口已同步至目标序号。
};

enum class Direction : std::uint8_t {
  request,        // 客户端 → 服务端，必有配对响应
  response,       // 服务端 → 客户端，配对响应
  push,           // 服务端 → 客户端，无配对响应
  bidirectional,  // 双向（heartbeat）
};

enum class ConnectionState : std::uint8_t {
  awaiting_hello,
  unauthenticated,
  authenticated,
};

enum class ConnectionStates : std::uint8_t {
  none = 0,
  awaiting_hello = 1 << 0,
  unauthenticated = 1 << 1,
  authenticated = 1 << 2,
  all = awaiting_hello | unauthenticated | authenticated,
};

constexpr ConnectionStates operator|(ConnectionStates a,ConnectionStates b) noexcept {
  return static_cast<ConnectionStates>(
    static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

constexpr bool hasState(ConnectionStates states, ConnectionState state) noexcept {
  // ConnectionState 是序号（0/1/2），必须先左移成位掩码（1/2/4）再按位与。
  const auto mask = static_cast<std::uint8_t>(
    1U << static_cast<std::uint8_t>(state));
  return (static_cast<std::uint8_t>(states) & mask) != 0;
}

// capability 取值（D197）："" 为基础协议能力；text_v1 门控文字内容；file_v1/voice_v1 首版不用。
struct ProtocolDescriptor {
  ProtocolType type;
  std::string_view name;
  Direction direction;
  ConnectionStates allowed_states;
  std::string_view capability;
  std::uint32_t max_body;            // 字节；D09 全局上限 65536；heartbeat 空正文为 0
  std::uint16_t default_timeout_ms;  // D74；0 = 无请求超时（响应／推送／心跳）
  std::optional<ProtocolType> response;
};

inline constexpr std::uint32_t kMaxJsonBody = 65536; // D09

inline constexpr std::array<ProtocolDescriptor, 54> kProtocolDescriptors{{
    // 系统与连接层
    {ProtocolType::protocol_error, "protocol_error", Direction::push,
     ConnectionStates::all, "", kMaxJsonBody, 0, {}},
    {ProtocolType::heartbeat, "heartbeat", Direction::bidirectional,
     ConnectionStates::unauthenticated | ConnectionStates::authenticated, "", 0, 0, {}},
    {ProtocolType::hello_request, "hello_request", Direction::request,
     ConnectionStates::awaiting_hello, "", kMaxJsonBody, 5000, ProtocolType::hello_response},
    {ProtocolType::hello_response, "hello_response", Direction::response,
     ConnectionStates::awaiting_hello, "", kMaxJsonBody, 0, {}},

    // 注册、登录、会话、重置（认证前）；退出与资料（认证后）
    {ProtocolType::register_prepare_request, "register_prepare_request", Direction::request,
     ConnectionStates::unauthenticated, "", kMaxJsonBody, 10000, ProtocolType::register_prepare_response},
    {ProtocolType::register_prepare_response, "register_prepare_response", Direction::response,
     ConnectionStates::unauthenticated, "", kMaxJsonBody, 0, {}},
    {ProtocolType::register_finalize_request, "register_finalize_request", Direction::request,
     ConnectionStates::unauthenticated, "", kMaxJsonBody, 10000, ProtocolType::register_finalize_response},
    {ProtocolType::register_finalize_response, "register_finalize_response", Direction::response,
      ConnectionStates::unauthenticated, "", kMaxJsonBody, 0, {}},

    {ProtocolType::login_request, "login_request", Direction::request,
     ConnectionStates::unauthenticated, "", kMaxJsonBody, 10000, ProtocolType::login_response},
    {ProtocolType::login_response, "login_response", Direction::response,
     ConnectionStates::unauthenticated, "", kMaxJsonBody, 0, {}},

    {ProtocolType::session_resume_request, "session_resume_request", Direction::request,
     ConnectionStates::unauthenticated, "", kMaxJsonBody, 10000, ProtocolType::session_resume_response},
    {ProtocolType::session_resume_response, "session_resume_response", Direction::response,
     ConnectionStates::unauthenticated, "", kMaxJsonBody, 0, {}},

    {ProtocolType::password_reset_request, "password_reset_request", Direction::request,
     ConnectionStates::unauthenticated, "", kMaxJsonBody, 10000, ProtocolType::password_reset_response},
    {ProtocolType::password_reset_response, "password_reset_response", Direction::response,
     ConnectionStates::unauthenticated, "", kMaxJsonBody, 0, {}},

    {ProtocolType::logout_request, "logout_request", Direction::request,
     ConnectionStates::authenticated, "", kMaxJsonBody, 2000, ProtocolType::logout_response},
    {ProtocolType::logout_response, "logout_response", Direction::response,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},

    {ProtocolType::profile_update_request, "profile_update_request", Direction::request,
     ConnectionStates::authenticated, "", kMaxJsonBody, 5000, ProtocolType::profile_update_response},
    {ProtocolType::profile_update_response, "profile_update_response", Direction::response,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},

    // 查找、申请与联系人
    {ProtocolType::user_lookup_request, "user_lookup_request", Direction::request,
     ConnectionStates::authenticated, "", kMaxJsonBody, 5000, ProtocolType::user_lookup_response},
    {ProtocolType::user_lookup_response, "user_lookup_response", Direction::response,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},

    {ProtocolType::friend_request_create_request, "friend_request_create_request", Direction::request,
     ConnectionStates::authenticated, "", kMaxJsonBody, 5000, ProtocolType::friend_request_create_response},
    {ProtocolType::friend_request_create_response, "friend_request_create_response", Direction::response,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},
    {ProtocolType::friend_request_list_request, "friend_request_list_request", Direction::request,
     ConnectionStates::authenticated, "", kMaxJsonBody, 10000, ProtocolType::friend_request_list_response},
    {ProtocolType::friend_request_list_response, "friend_request_list_response", Direction::response,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},
    {ProtocolType::friend_request_accept_request, "friend_request_accept_request", Direction::request,
     ConnectionStates::authenticated, "", kMaxJsonBody, 5000, ProtocolType::friend_request_accept_response},
    {ProtocolType::friend_request_accept_response, "friend_request_accept_response", Direction::response,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},
    {ProtocolType::friend_request_reject_request, "friend_request_reject_request", Direction::request,
     ConnectionStates::authenticated, "", kMaxJsonBody, 5000, ProtocolType::friend_request_reject_response},
    {ProtocolType::friend_request_reject_response, "friend_request_reject_response", Direction::response,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},
    {ProtocolType::friend_request_withdraw_request, "friend_request_withdraw_request", Direction::request,
     ConnectionStates::authenticated, "", kMaxJsonBody, 5000, ProtocolType::friend_request_withdraw_response},
    {ProtocolType::friend_request_withdraw_response, "friend_request_withdraw_response", Direction::response,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},
    {ProtocolType::friend_delete_request, "friend_delete_request", Direction::request,
     ConnectionStates::authenticated, "", kMaxJsonBody, 5000, ProtocolType::friend_delete_response},
    {ProtocolType::friend_delete_response, "friend_delete_response", Direction::response,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},

    {ProtocolType::contact_list_request, "contact_list_request", Direction::request,
     ConnectionStates::authenticated, "", kMaxJsonBody, 10000, ProtocolType::contact_list_response},
    {ProtocolType::contact_list_response, "contact_list_response", Direction::response,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},

    // 会话与文字消息
    {ProtocolType::message_send_request, "message_send_request", Direction::request,
     ConnectionStates::authenticated, "text_v1", kMaxJsonBody, 5000, ProtocolType::message_send_response},
    {ProtocolType::message_send_response, "message_send_response", Direction::response,
     ConnectionStates::authenticated, "text_v1", kMaxJsonBody, 0, {}},

    {ProtocolType::delivery_progress_request, "delivery_progress_request", Direction::request,
     ConnectionStates::authenticated, "", kMaxJsonBody, 5000, ProtocolType::delivery_progress_response},
    {ProtocolType::delivery_progress_response, "delivery_progress_response", Direction::response,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},

    {ProtocolType::conversation_list_request, "conversation_list_request", Direction::request,
     ConnectionStates::authenticated, "", kMaxJsonBody, 10000, ProtocolType::conversation_list_response},
    {ProtocolType::conversation_list_response, "conversation_list_response", Direction::response,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},
    {ProtocolType::conversation_history_request, "conversation_history_request", Direction::request,
     ConnectionStates::authenticated, "", kMaxJsonBody, 10000, ProtocolType::conversation_history_response},
    {ProtocolType::conversation_history_response, "conversation_history_response", Direction::response,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},
    {ProtocolType::conversation_read_request, "conversation_read_request", Direction::request,
     ConnectionStates::authenticated, "", kMaxJsonBody, 5000, ProtocolType::conversation_read_response},
    {ProtocolType::conversation_read_response, "conversation_read_response", Direction::response,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},
    {ProtocolType::conversation_clear_request, "conversation_clear_request", Direction::request,
     ConnectionStates::authenticated, "", kMaxJsonBody, 5000, ProtocolType::conversation_clear_response},
    {ProtocolType::conversation_clear_response, "conversation_clear_response", Direction::response,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},

    // 服务端推送
    {ProtocolType::session_terminated_push, "session_terminated_push", Direction::push,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},
    {ProtocolType::profile_updated_push, "profile_updated_push", Direction::push,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},
    {ProtocolType::friend_request_received_push, "friend_request_received_push", Direction::push,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},
    {ProtocolType::friendship_changed_push, "friendship_changed_push", Direction::push,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},
    {ProtocolType::friend_request_changed_push, "friend_request_changed_push", Direction::push,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},
    {ProtocolType::conversation_event_push, "conversation_event_push", Direction::push,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},
    {ProtocolType::message_received_push, "message_received_push", Direction::push,
     ConnectionStates::authenticated, "text_v1", kMaxJsonBody, 0, {}},
    {ProtocolType::delivery_sync_complete_push, "delivery_sync_complete_push", Direction::push,
     ConnectionStates::authenticated, "", kMaxJsonBody, 0, {}},
}};

constexpr const ProtocolDescriptor* findProtocol(ProtocolType type) noexcept {
  for (const auto& descriptor : kProtocolDescriptors) {
    if (descriptor.type == type) {
      return &descriptor;
    }
  }
  return nullptr;
}
}  // namespace chathub::contracts