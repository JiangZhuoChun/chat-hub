#include "net/session.h"
#include "protocol/chat_payload.h"

#include <boost/json.hpp>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <ctime>


namespace {
std::string formatUtcTimestamp(const std::chrono::system_clock::time_point time_point) {
  const std::time_t time = std::chrono::system_clock::to_time_t(time_point);
  std::tm utc_time{};
  if (gmtime_s(&utc_time,&time) != 0) {
      return "unavailable";
  }

  const auto milliseconds =
    std::chrono::duration_cast<std::chrono::milliseconds>(time_point.time_since_epoch()) % 1000;

  std::ostringstream stream;
  stream << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%S")
        << "."
        << std::setw(3)
        << std::setfill('0')
        << milliseconds.count()
        << "Z";
  return stream.str();
}
}

namespace net {

// ==================== 模块：生命周期与对外发送 ====================
// 功能：接管 Socket，创建会话 strand，并保存由 Server 注入的三个回调。
Session::Session(
    asio::ip::tcp::socket socket, SessionId session_id,
    std::chrono::milliseconds authentication_timeout,
    std::shared_ptr<auth::IAuthIntrospectionClient> auth_introspection_client,
    MessageCallback on_message, DisconnectCallback on_disconnect,
    AuthenticationRequestedCallback on_authentication_requested,
    AuthenticationTimeoutCallback on_authentication_timeout)
    : m_socket(std::move(socket)),
      m_strand(m_socket.get_executor()),
      m_authentication_timer(m_strand),
      m_authentication_timeout(authentication_timeout),
      m_auth_introspection_client(std::move(auth_introspection_client)),
      m_on_message(std::move(on_message)),
      m_id(session_id),
      m_on_disconnect(std::move(on_disconnect)),
      on_authentication_requested(std::move(on_authentication_requested)),
      m_on_authentication_timeout(std::move(on_authentication_timeout)) {}

// 功能：将第一次读取任务投递到会话 strand，确保异步回调开始前会话对象已被持有。
void Session::start() {
  const auto self = shared_from_this();
  asio::post(m_strand,
             // 功能：在会话串行执行器中启动持续读取流程。
             [self] {
               self->startAuthenticationDeadlineOnStrand();

               self->doRead();
             });
}

// 功能：将发送请求投递到会话 strand，保证写队列只被串行访问。
void Session::send(const protocol::MessageType type, std::string body) {
  const auto self = shared_from_this();
  asio::post(m_strand,
             // 功能：在会话串行执行器中将消息编码后压入写队列。
             [self, type, body = std::move(body)] {
               self->enqueueAndWrite(type, body);
             });
}

void Session::sendHistoryResultBodies(std::vector<std::string> bodies) {
  const auto self = shared_from_this();
  asio::post(m_strand, [self, bodies = std::move(bodies)]() mutable {
    for (std::string &body : bodies) {
      self->m_pending_history_result_bodies.push_back(std::move(body));
    }
    self->startNextHistoryResultBody();
  });
}

// 功能：将关闭请求投递到会话 strand，避免 Server 线程直接并发修改 Session
// 状态。
void Session::requestClose() {
  const auto self = shared_from_this();
  asio::post(m_strand, [self] { self->closeOnStrand(); });
}

// 功能：接收 Server
// 已确认有效的用户名与在线快照，原子地完成本会话的认证发送序列。
// 顺序：仅在认证待定时提交认证状态，并将 auth.ok 排在 online_users
// 之前入写队列。
void Session::completeAuthentication(std::string username,
                                     std::string online_users_body) {
  const auto self = shared_from_this();

  asio::post(m_strand, [self, username = std::move(username),
                        online_users_body = std::move(online_users_body),
                        this]() mutable {
    if (m_disconnected || !m_authentication_pending || m_close_after_write) {
      return;
    }
    if (online_users_body.size() > protocol::kMaxFrameBodyLength) {
      self->closeOnStrand();
      return;
    }

    cancelAuthenticationDeadlineOnStrand();

    m_username = std::move(username);
    m_authenticated = true;
    m_authentication_pending = false;
    enqueueAndWrite(protocol::MessageType::auth, "{\"ok\":true}");
    enqueueAndWrite(protocol::MessageType::online_users, online_users_body);
  });
}

// 功能：接收 Server 已确认有效的拒绝错误正文，写完 error 帧后再关闭会话。
// 边界：错误正文超过帧上限时保护性关闭；会话不再等待准入时直接忽略请求。
void Session::rejectAuthentication(std::string error_body,
                                   std::string error_code) {
  const auto self = shared_from_this();

  asio::post(m_strand, [self, error_body = std::move(error_body),
                        error_code = std::move(error_code)]() mutable {
    self->rejectAuthenticationOnStrand(error_body, error_code);
  });
}

void Session::startAuthenticationDeadlineOnStrand() {
  m_authentication_timer.expires_after(m_authentication_timeout);

  const auto self = shared_from_this();

  m_authentication_timer.async_wait(
      asio::bind_executor(m_strand, [self](const std::error_code &error) {
        self->handleAuthenticationDeadlineOnStrand(error);
      }));
}

void Session::cancelAuthenticationDeadlineOnStrand() {
  m_authentication_timer.cancel();
}

void Session::handleAuthenticationDeadlineOnStrand(
    const std::error_code &error) {
  // 定时器被主动取消，不是真正的认证超时
  if (error == asio::error::operation_aborted) {
    return;
  }
  if (error) {
    log("auth", "authentication_deadline_wait_failed", "timer_wait_failed");
    closeOnStrand();
    return;
  }

  // Session 已经进入其他终态，不再处理认证超时
  if (m_disconnected || m_authenticated || m_close_after_write) {
    return;
  }
  // 保持“等待认证结论”状态，并把终态裁决交给 Server strand。
  m_authentication_pending = true;

  if (m_on_authentication_timeout) {
    m_on_authentication_timeout(m_id);
  } else {
    closeOnStrand();
  }
}
void Session::handleIntrospectionResultOnStrand(
    auth::IntrospectionResult result) {
  m_auth_introspection_request.reset();

  // 迟到的 introspection 结果不再改变已经终态的 Session。
  if (m_disconnected || !m_authentication_pending || m_close_after_write) {
    return;
  }
  switch (result.status) {
    case auth::IntrospectionStatus::active: {
      // Auth Service 返回的身份仍必须符合 ChatHub 本地用户名合同。
      if (!protocol::isValidUsername(result.username)) {
        // 保留 W10 已有的 wire error 合同：身份 claim 不合规是认证拒绝，
        // 不能把一个可确定的身份格式错误伪装成依赖暂时不可用。
        rejectAuthenticationOnStrand(
            makeAuthError("invalid_username_claim", "认证失败"),
            "invalid_username_claim");
        return;
      }
      if (on_authentication_requested) {
        on_authentication_requested(m_id, result.username);
      } else {
        // Server 回调缺失属于组装/依赖故障。
        rejectAuthenticationOnStrand(
            makeAuthError("authentication_dependency_unavailable",
                          "认证服务暂时不可用"),
            "authentication_dependency_unavailable");
      }
      return;
    }
    case auth::IntrospectionStatus::authentication_rejected: {
      // 已确定 token 无效，属于凭证拒绝。
      rejectAuthenticationOnStrand(
          makeAuthError("authentication_rejected", "认证失败"),
          "authentication_rejected");
      return;
    }
    case auth::IntrospectionStatus::dependency_unavailable: {
      log("auth", "introspection_dependency_unavailable", result.error_code);
      // 无法安全判断 token，必须 fail-closed。
      rejectAuthenticationOnStrand(
          makeAuthError("authentication_dependency_unavailable",
                        "认证服务暂时不可用"),
          "authentication_dependency_unavailable");
      return;
    }
    default:
      // 未来新增未知状态时，也不能默认放行。
      rejectAuthenticationOnStrand(
          makeAuthError("authentication_dependency_unavailable",
                        "认证服务暂时不可用"),
          "authentication_dependency_unavailable");
  }
}

void Session::rejectAuthenticationOnStrand(const std::string &error_body,
                                           const std::string &error_code) {
  if (m_disconnected || !m_authentication_pending || m_close_after_write) {
    return;
  }
  if (error_body.size() > protocol::kMaxFrameBodyLength) {
    closeOnStrand();
    return;
  }
  log("auth", "authentication_rejected", error_code);
  // 记录“这一批队列写完后要关闭”，但此刻不关，确保 error 帧先完成异步写入。
  m_close_after_write = true;
  enqueueAndWrite(protocol::MessageType::error, error_body, true);
}

// ==================== 模块：异步读取与帧解码 ====================
// 功能：异步读取 Socket 数据，解码完整帧并递归安排下一次读取。
// 失败：读取错误或协议错误时关闭会话，触发 Server 的在线表清理回调。
void Session::doRead() {
  const auto self = shared_from_this();
  m_socket.async_read_some(
      asio::buffer(m_read_buffer),
      asio::bind_executor(
          m_strand,
          // 功能：处理本次读取结果、协议解码结果并决定是否继续读取。
          [self, this](const std::error_code error,
                       const std::size_t bytes_transferred) {
            if (error) {
              if (error == asio::error::eof) {
                self->log("read", "peer_disconnected", "normal_disconnect");
              } else if (error == asio::error::operation_aborted) {
                return;
              } else {
                self->log("read", "socket_read_failed", "socket_read_failed");
              }
              self->closeOnStrand();
              return;
            }
            const auto result = m_decoder.append(
                m_read_buffer.data(), bytes_transferred,
                // 功能：将每条完整协议消息交给当前会话的业务分派函数。
                [self](const protocol::Message &message) {
                  self->handlerMessage(message);
                });
            if (result != protocol::DecodeResult::ok) {
              log("read", "frame_decode_rejected", "frame_decode_failed");
              self->closeOnStrand();
              return;
            }

            self->doRead();
          }));
}

// ==================== 模块：串行写队列 ====================
// 功能：校验正文大小后将完整帧放入队列；空队列首次入队时启动异步写。
void Session::enqueueAndWrite(const protocol::MessageType type,
                              const std::string &body,
                              const bool allow_terminal_overflow) {
  if (body.size() > protocol::kMaxFrameBodyLength) {
    log("write", "outbound_frame_rejected", "frame_body_too_large", body.size(),
        protocol::kMaxFrameBodyLength);
    return;
  }

  const bool was_empty = m_write_queue.empty();
  if (m_write_queue.size() >= kMaxWriteQueueSize && !allow_terminal_overflow) {
    log("write", "slow_client_disconnected", "write_queue_full",
        m_write_queue.size(), kMaxWriteQueueSize);

    closeOnStrand();
    return;
  }

  m_write_queue.push_back({type, protocol::makeFrame(type, body)});
  if (was_empty) {
    writeFrame();
  }
}

// 功能：异步写出队首帧；写入成功后移除队首并继续处理剩余队列。
//       若认证拒绝要求写完后关闭，则只在队列排空后执行关闭。
// 失败：写入失败时关闭会话，避免继续向失效 Socket 发送数据。
void Session::writeFrame() {
  if (m_write_queue.empty()) {
    return;
  }

  const auto self = shared_from_this();
  asio::async_write(
      m_socket, asio::buffer(m_write_queue.front().frame),
      asio::bind_executor(
          m_strand,
          // 功能：处理当前队首帧的写入结果，并按顺序继续下一个帧。
          [self, this](const std::error_code error,
                       const std::size_t bytes_transferred) {
            if (error) {
              self->log("write", "socket_write_failed", "socket_write_failed");
              self->closeOnStrand();
              return;
            }

            m_write_queue.pop_front();

            if (m_write_queue.empty()) {
              if (self->m_close_after_write) {
                // error 已写完且队列排空；此时才可关闭 Socket，不能截断错误帧。
                self->closeOnStrand();
                return;
              }
              self->startNextHistoryResultBody();
            } else {
              self->writeFrame();
            }
          }));
}
// 功能：从挂起列表中取出下一个历史结果正文并发送
void Session::startNextHistoryResultBody() {
  if (!m_write_queue.empty() || m_pending_history_result_bodies.empty()) {
    return;
  }
  const std::string body = std::move(m_pending_history_result_bodies.front());
  m_pending_history_result_bodies.pop_front();
  enqueueAndWrite(protocol::MessageType::history_result, body);
}

// 功能：构造包含聊天错误范围、错误码、错误说明和可选 local_id 的 JSON 正文。
std::string Session::makeChatError(const std::string &local_id,
                                   const std::string &code,
                                   const std::string &message) {
  boost::json::object object;
  object["scope"] = "chat";
  object["code"] = code;
  object["message"] = message;
  if (!local_id.empty()) {
    object["local_id"] = local_id;
  }
  return boost::json::serialize(object);
}

std::string Session::makeAuthError(const std::string &code,
                                   const std::string &message) {
  boost::json::object object;
  object["scope"] = "auth";
  object["code"] = code;
  object["message"] = message;

  return boost::json::serialize(object);
}

// 功能：在未认证时只处理认证帧；认证完成后分派聊天、心跳和错误帧。
//       若拒绝错误正在发送，则不再处理任何新入站帧。
// 失败：令牌无效、未认证发送业务帧或聊天正文校验失败时关闭会话或返回错误帧。
void Session::handlerMessage(const protocol::Message &message) {
  if (m_close_after_write) {
    // 已决定发送拒绝错误并关闭；忽略后续入站帧，防止提前关闭而截断 error。
    return;
  }

  if (m_authentication_pending) {
    log("auth", "additional_frame_ignored", "authentication_pending");
    return;
  }

  if (!m_authenticated) {
    if (message.type == protocol::MessageType::auth) {
      m_authentication_pending = true;

      if (!m_auth_introspection_client) {
        rejectAuthenticationOnStrand(
            makeAuthError("authentication_dependency_unavailable",
                          "认证服务暂时不可用"),
            "authentication_dependency_unavailable");
        return;
      }

      const std::weak_ptr<Session> weak_self = shared_from_this();
      m_auth_introspection_request = m_auth_introspection_client->introspect(
          message.body, [weak_self](auth::IntrospectionResult result) mutable {
            const auto self = weak_self.lock();
            if (!self) return;

            asio::post(self->m_strand, [weak_self,
                                        result = std::move(result)]() mutable {
              const auto self = weak_self.lock();
              if (!self) return;

              self->handleIntrospectionResultOnStrand(std::move(result));
            });
          });
      return;
    }

    // 心跳不是认证行为：忽略它但不重置认证截止。
    if (message.type == protocol::MessageType::ping ||
        message.type == protocol::MessageType::pong) {
      log("auth", "heartbeat_ignored", "unauthenticated_heartbeat");
      return;
    }

    if (message.type == protocol::MessageType::history_query) {
      send(protocol::MessageType::error,
           makeHistoryError("authentication_required",
                            "历史查询需要先完成认证"));
    } else {
      send(protocol::MessageType::error,
           makeChatError("", "authentication_required", "未认证"));
      closeOnStrand();
    }
    return;
  }

  switch (message.type) {
    case protocol::MessageType::chat: {
      const auto result = protocol::parseChatPayload(message.body);
      if (result.error != protocol::ChatPayloadError::none) {
        std::string error_message = "聊天消息校验失败";
        std::string error_code = "chat_validation_failed";
        switch (result.error) {
          case protocol::ChatPayloadError::none:
            break;
          case protocol::ChatPayloadError::invalid_json:
            error_message = "聊天 JSON 格式错误";
            error_code = "invalid_json";
            break;
          case protocol::ChatPayloadError::missing_content:
            error_message = "聊天消息缺少 content";
            error_code = "missing_content";
            break;
          case protocol::ChatPayloadError::content_not_string:
            error_message = "content 必须是字符串";
            error_code = "content_not_string";
            break;
          case protocol::ChatPayloadError::blank_content:
            error_message = "聊天内容不能为空";
            error_code = "blank_content";
            break;
          case protocol::ChatPayloadError::forbidden_sender_id:
            error_message = "客户端不能指定 sender_id";
            error_code = "forbidden_sender_id";
            break;
          case protocol::ChatPayloadError::content_too_long:
            error_message = "聊天内容不能超过 1024 字节";
            error_code = "content_too_long";
            break;
          case protocol::ChatPayloadError::missing_local_id:
            error_message = "聊天消息缺少 local_id";
            error_code = "missing_local_id";
            break;
          case protocol::ChatPayloadError::local_id_not_string:
            error_message = "local_id 必须是字符串";
            error_code = "local_id_not_string";
            break;
          case protocol::ChatPayloadError::blank_local_id:
            error_message = "local_id 不能为空";
            error_code = "blank_local_id";
            break;
          case protocol::ChatPayloadError::local_id_too_long:
            error_message = "local_id 不能超过 64 字节";
            error_code = "local_id_too_long";
            break;
          case protocol::ChatPayloadError::missing_recipient:
            error_message = "聊天消息缺少 to";
            error_code = "missing_recipient";
            break;
          case protocol::ChatPayloadError::recipient_not_string:
            error_message = "to 必须是字符串";
            error_code = "recipient_not_string";
            break;
          case protocol::ChatPayloadError::blank_recipient:
            error_message = "to 不能为空";
            error_code = "blank_recipient";
            break;
          case protocol::ChatPayloadError::missing_send_at:
            error_message = "聊天消息缺少 send_at";
            error_code = "missing_send_at";
            break;
          case protocol::ChatPayloadError::send_at_not_string:
            error_message = "send_at 必须是字符串";
            error_code = "send_at_not_string";
            break;
          case protocol::ChatPayloadError::blank_send_at:
            error_message = "send_at 不能为空";
            error_code = "blank_send_at";
            break;
          case protocol::ChatPayloadError::send_at_too_long:
            error_message = "send_at 不能超过 64 字节";
            error_code = "send_at_too_long";
            break;
        }
        send(protocol::MessageType::error,
             makeChatError(result.local_id, error_code, error_message));
        break;
      }
      m_on_message(m_id, message);
      break;
    }
    case protocol::MessageType::ping:
      send(protocol::MessageType::pong, message.body);
      break;
    case protocol::MessageType::pong:
      break;
    case protocol::MessageType::error: {
      log("dispatch", "peer_error_ignored", "inbound_error");
      break;
    }
    case protocol::MessageType::auth:
      break;
    case protocol::MessageType::chat_ack:
      break;
    case protocol::MessageType::delivery_receipt:
      m_on_message(m_id, message);
      break;
    case protocol::MessageType::online_users:
      break;
    case protocol::MessageType::history_query: {
      const auto result = protocol::parseHistoryQueryPayload(message.body);

      if (result.error != protocol::HistoryQueryPayloadError::none) {
        std::string error_code = "history_validation_failed";
        std::string error_message = "历史查询消息校验失败";

        switch (result.error) {
          case protocol::HistoryQueryPayloadError::none:
            break;
          case protocol::HistoryQueryPayloadError::invalid_json:
            error_code = "invalid_json";
            error_message = "历史查询 JSON 格式错误";
            break;
          case protocol::HistoryQueryPayloadError::forbidden_identity_field:
            error_code = "forbidden_identity_field";
            error_message = "历史查询消息包含禁止的 identity 字段";
            break;
          case protocol::HistoryQueryPayloadError::missing_request_id:
            error_code = "missing_request_id";
            error_message = "历史查询消息缺少 request_id";
            break;
          case protocol::HistoryQueryPayloadError::request_id_not_string:
            error_code = "request_id_not_string";
            error_message = "历史查询消息的 request_id 必须是字符串";
            break;
          case protocol::HistoryQueryPayloadError::blank_request_id:
            error_code = "blank_request_id";
            error_message = "历史查询消息的 request_id 不能为空";
            break;
          case protocol::HistoryQueryPayloadError::request_id_too_long:
            error_code = "request_id_too_long";
            error_message = "历史查询消息的 request_id 不能超过 64 字节";
            break;
          case protocol::HistoryQueryPayloadError::missing_limit:
            error_code = "missing_limit";
            error_message = "历史查询消息缺少 limit";
            break;
          case protocol::HistoryQueryPayloadError::limit_not_integer:
            error_code = "limit_not_integer";
            error_message = "历史查询消息的 limit 必须是整数";
            break;
          case protocol::HistoryQueryPayloadError::before_not_object:
            error_code = "before_not_object";
            error_message = "历史查询消息的 before 必须是对象";
            break;
          case protocol::HistoryQueryPayloadError::missing_before_timestamp:
            error_code = "missing_before_timestamp";
            error_message = "历史查询消息缺少 before.timestamp";
            break;
          case protocol::HistoryQueryPayloadError::before_timestamp_not_integer:
            error_code = "before_timestamp_not_integer";
            error_message = "历史查询消息的 before.timestamp 必须是整数";
            break;
          case protocol::HistoryQueryPayloadError::negative_before_timestamp:
            error_code = "negative_before_timestamp";
            error_message = "历史查询消息的 before.timestamp 不能为负数";
            break;
          case protocol::HistoryQueryPayloadError::missing_before_message_id:
            error_code = "missing_before_message_id";
            error_message = "历史查询消息缺少 before.message_id";
            break;
          case protocol::HistoryQueryPayloadError::before_message_id_not_string:
            error_code = "before_message_id_not_string";
            error_message = "历史查询消息的 before.message_id 必须是字符串";
            break;
          case protocol::HistoryQueryPayloadError::blank_before_message_id:
            error_code = "blank_before_message_id";
            error_message = "历史查询消息的 before.message_id 不能为空";
            break;
          case protocol::HistoryQueryPayloadError::before_message_id_too_long:
            error_code = "before_message_id_too_long";
            error_message = "历史查询消息的 before.message_id 不能超过 64 字节";
            break;
        }
        send(protocol::MessageType::error,
             makeHistoryError(error_code, error_message));
        break;
      }
      // 到这里说明请求体已经通过协议校验。
      m_on_message(m_id, message);
      break;
    }
    default:;
  }
}

std::string Session::makeHistoryError(const std::string &code,
                                      const std::string &message) {
  boost::json::object object;
  object["scope"] = "history";
  object["code"] = code;
  object["message"] = message;

  return boost::json::serialize(object);
}

// ==================== 模块：关闭与日志 ====================
// 功能：仅第一次调用时标记会话已断开，并通知 Server 清理对应会话记录。
void Session::closeOnStrand() {
  if (m_disconnected) {
    return;
  }
  m_disconnected = true;

  cancelAuthenticationDeadlineOnStrand();

  if (m_auth_introspection_request) {
    m_auth_introspection_request->cancel();
    m_auth_introspection_request.reset();
  }

  // 优雅地关闭 socket
  std::error_code ignored_error;
  m_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored_error);
  m_socket.close(ignored_error);

  m_on_disconnect(m_id);
}

// 功能：统一输出会话生命周期与协议处理日志。
void Session::log(std::string_view phase, std::string_view event,
                  std::string_view code, std::optional<std::size_t> actual,
                  std::optional<std::size_t> limit) const {
  // 1.先在局部变量中拼完整条日志
  std::ostringstream oss;
  oss << "timestamp=" << formatUtcTimestamp(std::chrono::system_clock::now())
      << " session_id=" << m_id
      << " component=chat_server"
      << " phase=" << phase
      << " event=" << event
      << " code=" << code;
  // 2. optional 有值时才输出
  if (actual.has_value()) {
    oss << " actual=" << *actual;
  }
  if (limit.has_value()) {
    oss << " limit=" << *limit;
  }
  // 3. 函数内 static：所有 log() 调用共享同一把锁
  static std::mutex log_mutex;

  // 4. 只保护最终输出
  {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cout << oss.str() << '\n';
  }
}

}  // namespace net
