#include "net/server.h"
#include "net/session.h"
#include "protocol/chat_payload.h"
#include "auth/asio_auth_introspection_client.h"

#include <boost/json.hpp>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>
namespace
{

// ==================== 模块：路由错误正文构造 ====================
// 功能：构造包含 local_id 的聊天错误 JSON，使客户端能定位发送失败的消息气泡。
std::string makeRouteErrorBody(const std::string &local_id, const std::string &code, const std::string &message)
{
    boost::json::object object;
    object["scope"] = "chat";
    object["code"] = std::string(code);
    object["message"] = std::string(message);
    if (!local_id.empty())
    {
        object["local_id"] = std::string(local_id);
    }
    return boost::json::serialize(object);
}

// 功能：构造包含 code 和 message 的送达回执错误 JSON，用于客户端请求送达回执失败时返回。
std::string makeDeliveryReceiptErrorBody(const std::string &code, const std::string &message)
{
    boost::json::object object;
    object["scope"] = "delivery_receipt";
    object["code"] = std::string(code);
    object["message"] = std::string(message);
    return boost::json::serialize(object);
}

// 生成 history 错误
std::string makeHistoryError(const std::string &code, const std::string &message)
{
    boost::json::object object;
    object["scope"] = "history";
    object["code"] = code;
    object["message"] = message;

    return boost::json::serialize(object);
}

// “数据层记录 → 网络协议 JSON 记录”的转换器
boost::json::object makeHistoryMessageObject(const repository::StoredMessage &message)
{
    boost::json::object object;
    object["message_id"] = message.message_id;
    object["local_id"] = message.client_local_id;
    object["from"] = message.sender;
    object["to"] = message.recipient;
    object["content"] = message.content;
    object["send_at"] = message.client_send_at;
    object["server_received_at_ms"] = message.server_received_at_ms;
    return object;
}

// 把 query_result.messages 切成实际 JSON body 不超过 2048 字节的 history_result 块
std::string makeHistoryResultBody(const std::string &request_id, const boost::json::array &messages,
                                  const bool is_last_chunk, const repository::HistoryQueryResult &query_result)
{
    boost::json::object object;
    object["request_id"] = request_id;
    object["messages"] = messages;
    object["is_last_chunk"] = is_last_chunk;

    if (is_last_chunk)
    {
        object["has_more"] = query_result.has_more;

        if (query_result.has_more && query_result.next_cursor.has_value())
        {
            boost::json::object cursor;
            cursor["server_received_at_ms"] = query_result.next_cursor->server_received_at_ms;
            cursor["message_id"] = query_result.next_cursor->message_id;
            object["next_cursor"] = std::move(cursor);
        }
        else
        {
            object["next_cursor"] = nullptr;
        }
    }
    return boost::json::serialize(object);
}

std::string makeOnlineUsersCapacityErrorBody()
{
    boost::json::object obj;
    obj["scope"] = "online_users";
    obj["code"] = "online_snapshot_capacity_exceeded";
    obj["message"] = "在线用户快照容量已满";
    obj["max_users"] = protocol::kMaxOnlineUsersSnapshotCount;
    return boost::json::serialize(obj);
}

// 功能：构造认证截止的稳定错误正文，供 Server strand 决定拒绝后交给 Session 写出。
std::string makeAuthenticationTimeoutErrorBody()
{
    boost::json::object obj;
    obj["scope"] = "auth";
    obj["code"] = "authentication_timeout";
    obj["message"] = "认证超时";
    return boost::json::serialize(obj);
}

std::optional<std::string> generateMessageId()
{
    constexpr char kHexDigits[] = "0123456789ABCDEF";
    std::array<unsigned char, 16> random_bytes{};

    if (RAND_bytes(random_bytes.data(), static_cast<int>(random_bytes.size())) != 1)
    {
        return std::nullopt;
    }

    std::string message_id;
    message_id.reserve(random_bytes.size() * 2);

    for (const unsigned char byte : random_bytes)
    {
        message_id.push_back(kHexDigits[byte >> 4]);
        message_id.push_back(kHexDigits[byte & 0x0F]);
    }

    return message_id;
}
} // namespace

namespace net
{

// ==================== 模块：生命周期与监听 ====================
// 功能：创建 Server 串行执行器，绑定 IPv4 监听端口，并准备异步接受使用的 Socket。
Server::Server(asio::io_context &io_context, const std::uint16_t port, std::string database_path,
               const std::chrono::milliseconds authentication_timeout,
               auth::AuthIntrospectionConfig auth_introspection_config,
               std::unique_ptr<repository::IMessageRepository> message_repository,
               std::shared_ptr<auth::IAuthIntrospectionClient>
                   auth_introspection_client)
    : m_strand(asio::make_strand(io_context)),
      m_acceptor(io_context, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)), m_pending_socket(io_context),
      m_database_path(std::move(database_path)), m_authentication_timeout(authentication_timeout),
      m_auth_introspection_config(std::move(auth_introspection_config)),
      m_auth_introspection_client(std::move(auth_introspection_client)),
      m_message_repository(std::move(message_repository)), m_database_available(m_message_repository != nullptr)
{

    if (!m_auth_introspection_client)
    {
        m_auth_introspection_client =
            std::make_shared<auth::AsioAuthIntrospectionClient>(io_context, m_auth_introspection_config);
    }
    if (!m_database_available)
    {
        std::cerr << "SQLite 初始化失败：聊天持久化暂不可用" << std::endl;
    }
}

// 功能：输出当前监听地址并启动第一个异步接受操作。
void Server::start()
{
    std::cout << "server_started"
              << " endpoint=" << m_acceptor.local_endpoint() << " database_path=" << std::quoted(m_database_path)
              << " auth_timeout_ms=" << m_authentication_timeout.count() << std::endl;
    doAccept();
}

// 功能：持续接受新连接；每次回调结束前重建待接受 Socket 并再次监听。
// 失败：接受失败时记录错误，但不会停止后续接受操作。
void Server::doAccept()
{
    m_acceptor.async_accept(
        m_pending_socket,
        asio::bind_executor(
            m_strand,
            // 功能：处理一次连接接受结果，并为成功连接创建、登记和启动 Session。
            [this](const std::error_code error) {
                if (error == asio::error::operation_aborted)
                {
                    return;
                }

                if (error)
                {
                    std::cerr << "接受连接失败：" << error.message();
                }
                else
                {
                    const SessionId session_id = m_next_session_id++;
                    std::error_code endpoint_error;
                    const auto endpoint = m_pending_socket.remote_endpoint(endpoint_error);
                    if (endpoint_error)
                    {
                        std::cerr << "客户端#" << session_id << "地址读取失败：" << endpoint_error.message()
                                  << std::endl;
                    }
                    else
                    {
                        std::cout << "客户端#" << session_id << "已连接：" << endpoint << std::endl;
                    }

                    // 功能：将 Session 消息投递回 Server strand，再访问路由表。
                    auto on_message = [this](SessionId sender_id, protocol::Message message) {
                        asio::post(
                            m_strand,
                            // 功能：在 Server strand 中按发送者会话标识路由聊天消息。
                            [this, sender_id, message = std::move(message)] { onSessionMessage(sender_id, message); });
                    };

                    // 功能：将 Session 断开事件投递回 Server strand，统一清理会话表。
                    auto on_disconnect = [this](const SessionId disconnected_session_id) {
                        asio::post(m_strand,
                                   // 功能：在 Server strand 中移除已断开会话的所有映射。
                                   [this, disconnected_session_id] { removeSession(disconnected_session_id); });
                    };

                    // 功能：将认证成功事件投递回 Server strand，登记用户名与会话映射。
                    auto on_authentication_requested = [this](SessionId session_id, std::string username) {
                        asio::post(m_strand,
                                   // 功能: 注册已认证会话并广播在线用户列表。
                                   [this, session_id, username = std::move(username)] {
                                       handleAuthenticationRequest(session_id, username);
                                   });
                    };

                    auto on_authentication_timeout = [this](const SessionId timed_out_session_id) {
                        asio::post(m_strand,
                                   [this, timed_out_session_id] { handleAuthenticationTimeout(timed_out_session_id); });
                    };

                    const auto session = std::make_shared<Session>(
                        std::move(m_pending_socket), session_id, m_authentication_timeout,
                        m_auth_introspection_client,
                        std::move(on_message),
                        std::move(on_disconnect), std::move(on_authentication_requested),
                        std::move(on_authentication_timeout));
                    addSession(session_id, session);
                    session->start();
                }

                m_pending_socket = asio::ip::tcp::socket(m_acceptor.get_executor());
                doAccept();
            }));
}

// ==================== 模块：会话登记与清理 ====================
// 功能：将新会话加入在线会话表，并输出当前在线数量。
void Server::addSession(const SessionId session_id, const SessionPtr &session)
{
    m_sessions.emplace(session_id, session);
    std::cout << "客户端#" << session_id << "已登记,当前在线:" << m_sessions.size() << std::endl;
}

// 功能：删除会话表、用户名到会话表和会话到用户名表中的断开连接记录。
void Server::removeSession(const SessionId session_id)
{
    // 只有该用户名确实没有被新会话接管时才保存它：
    // 非空表示“接收者已离线”，供待送达记录按接收者整组清理。
    std::string recipient_username_to_clean;

    // 断开事件只带 session_id，先通过反向映射找出旧会话原本的用户名。
    if (const auto reverse_it = m_session_to_username.find(session_id); reverse_it != m_session_to_username.end())
    {
        const std::string username = reverse_it->second;

        // 同名新连接可能已把 username 指向另一个 SessionId。
        // 只有正向映射仍指向本次断开的旧会话，才说明该用户名真的离线。
        if (const auto forward_it = m_username_to_session.find(username);
            forward_it != m_username_to_session.end() && forward_it->second == session_id)
        {
            m_username_to_session.erase(forward_it);
            recipient_username_to_clean = username;
        }
        // 无论是否被新会话接管，这条旧 session_id -> username 反向映射都已失效。
        m_session_to_username.erase(reverse_it);
    }

    // 始终按旧会话 ID 清理其作为发送者的记录；
    // 只有普通断开才传入用户名，避免顶替场景误删新会话作为接收者的记录。
    removePendingDeliveriesForSession(session_id, recipient_username_to_clean);
    if (const auto rm_count = m_sessions.erase(session_id); rm_count != 0)
    {
        std::cout << "客户端 #" << session_id << " 已移出在线表，当前在线：" << m_sessions.size() << std::endl;
    }

    if (!recipient_username_to_clean.empty())
    {
        broadcastOnlineUsers();
    }
}

// ==================== 模块：聊天消息路由 ====================
// 功能：记录收到的业务消息并交给私聊路由逻辑。
void Server::onSessionMessage(const SessionId sender_id, const protocol::Message &message)
{
    switch (message.type)
    {
    case protocol::MessageType::chat:
        sendToUser(sender_id, message);
        break;
    case protocol::MessageType::delivery_receipt:
        handleDeliveryReceipt(sender_id, message);
        break;
    case protocol::MessageType::history_query:
        handleHistoryQuery(sender_id, message);
        break;
    default:
        break;
    }
}

// 功能：检查发送者和接收者在线状态，转发聊天帧并向发送者发送确认帧。
// 失败：发送者未登记或接收者离线时，仅向发送者发送带 local_id 的错误帧。
void Server::sendToUser(const SessionId sender_id, const protocol::Message &message)
{
    // 1. 解析消息正文
    const auto payload = protocol::parseChatPayload(message.body);
    if (payload.error != protocol::ChatPayloadError::none)
    {
        return;
    }
    const auto sender_it = m_sessions.find(sender_id);
    const auto sender_username_it = m_session_to_username.find(sender_id);
    const auto target_it = m_username_to_session.find(payload.to);
    // 2. 确认发送 Session 还存在
    if (sender_it == m_sessions.end())
    {
        return;
    }
    // 3. 确认它有认证身份
    if (sender_username_it == m_session_to_username.end())
    {
        const std::string error_body =
            makeRouteErrorBody(payload.local_id, "sender_not_registered", "发送者会话尚未完成注册");
        sender_it->second->send(protocol::MessageType::error, error_body);
        return;
    }
    // 4. 确认它仍是该身份的当前活动会话
    if (!isCurrentAuthenticatedSession(sender_id, sender_username_it->second))
    {
        sender_it->second->send(protocol::MessageType::error,
                                makeRouteErrorBody(payload.local_id, "session_replaced", "当前登录已在其他连接接管"));
        return;
    }
    // 5. 确认接收者在线
    if (target_it == m_username_to_session.end())
    {
        const std::string error_body = makeRouteErrorBody(payload.local_id, "recipient_offline", "接收者不在线");
        sender_it->second->send(protocol::MessageType::error, error_body);
        return;
    }
    // 6. 确认接收 Session 还存在
    const auto recv_it = m_sessions.find(target_it->second);
    if (recv_it == m_sessions.end())
    {
        const std::string error_body = makeRouteErrorBody(payload.local_id, "recipient_offline", "接收者连接已经断开");
        sender_it->second->send(protocol::MessageType::error, error_body);
        return;
    }
    // 7. 确认数据库可用
    if (!m_database_available)
    {
        const std::string error_body =
            makeRouteErrorBody(payload.local_id, "database_unavailable", "数据库当前不可用，消息未发送");
        sender_it->second->send(protocol::MessageType::error, error_body);
        return;
    }
    // 8. 生成候选持久身份并交给 Repository 写入
    std::string sender = sender_username_it->second;
    std::string recipient = payload.to;
    std::string content = payload.content;
    std::string local_id = payload.local_id;
    std::string send_at = payload.send_at;
    auto now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();

    const auto candidate_message_id = generateMessageId();
    if (!candidate_message_id.has_value())
    {
        std::cerr << "message_id_generation_failed\n";
        sender_it->second->send(protocol::MessageType::error,
                                makeRouteErrorBody(payload.local_id, "database_write_failed", "数据库错误"));
        return;
    }

    repository::NewMessage msg{sender, recipient, content, send_at, local_id, now_ms, *candidate_message_id};
    auto outcome = m_message_repository->storeMessage(msg);
    switch (outcome.result)
    {
    case repository::StoreResult::Stored:
        break;
    case repository::StoreResult::DuplicateSame: {
        boost::json::object ack;
        ack["message_id"] = outcome.message_id;
        ack["local_id"] = payload.local_id;
        ack["status"] = "accepted";
        ack["server_received_at_ms"] = outcome.server_received_at_ms;
        sender_it->second->send(protocol::MessageType::chat_ack, boost::json::serialize(ack));
        return;
    }
    case repository::StoreResult::IdempotencyConflict:
        sender_it->second->send(protocol::MessageType::error,
                                makeRouteErrorBody(payload.local_id, "idempotency_conflict", "消息部分冲突"));
        return;
    case repository::StoreResult::DatabaseError:
        sender_it->second->send(protocol::MessageType::error,
                                makeRouteErrorBody(payload.local_id, "database_write_failed", "数据库错误"));
        return;
    }
    // 9. 以本次持久化返回的 message_id 登记待送达记录，避免回执关联到其他消息。
    if (!rememberPendingDelivery(outcome.message_id, sender_username_it->second, sender_id, payload.local_id,
                                 payload.to))
    {
        const std::string error_body =
            makeRouteErrorBody(payload.local_id, "pending_delivery_register_failed", "消息已保存，但送达状态登记失败");
        sender_it->second->send(protocol::MessageType::error, error_body);
        return;
    }

    boost::json::object forwarded;
    forwarded["message_id"] = outcome.message_id;
    forwarded["local_id"] = payload.local_id;
    forwarded["from"] = sender_username_it->second;
    forwarded["to"] = payload.to;
    forwarded["content"] = payload.content;
    forwarded["send_at"] = payload.send_at;
    forwarded["server_received_at_ms"] = outcome.server_received_at_ms;
    const std::string forward_body = boost::json::serialize(forwarded);
    recv_it->second->send(protocol::MessageType::chat, forward_body);

    boost::json::object ack;
    ack["local_id"] = payload.local_id;
    ack["status"] = "accepted";
    ack["message_id"] = outcome.message_id;
    ack["server_received_at_ms"] = outcome.server_received_at_ms;
    sender_it->second->send(protocol::MessageType::chat_ack, boost::json::serialize(ack));
}

// 功能：校验接收者回执的 message_id 与认证身份，并将最终送达状态通知原发送者。
void Server::handleDeliveryReceipt(const SessionId receipt_sender_id, const protocol::Message &message)
{
    const auto sender_it = m_sessions.find(receipt_sender_id);
    if (sender_it == m_sessions.end())
    {
        return;
    }

    const protocol::DeliveryReceiptPayloadResult payload_result = protocol::parseDeliveryReceiptPayload(message.body);

    if (payload_result.error != protocol::DeliveryReceiptPayloadError::none)
    {
        const std::string error_body = makeDeliveryReceiptErrorBody("invalid_delivery_receipt", "送达回执格式错误");
        sender_it->second->send(protocol::MessageType::error, error_body);
        return;
    }
    // 这是理论上不应出现的防御性情况
    const auto username_it = m_session_to_username.find(receipt_sender_id);
    if (username_it == m_session_to_username.end())
    {
        return;
    }

    // 确认它仍是该身份的当前活动会话
    if (!isCurrentAuthenticatedSession(receipt_sender_id, username_it->second))
    {
        sender_it->second->send(protocol::MessageType::error,
                                makeDeliveryReceiptErrorBody("session_replaced", "当前登录已在其他连接接管"));
        return;
    }

    const auto delivery_it = m_pendingDeliveries.find(payload_result.message_id);
    if (delivery_it == m_pendingDeliveries.end() || delivery_it->second.recipient_username != username_it->second)
    {
        const std::string error_body = makeDeliveryReceiptErrorBody("unknown_delivery_receipt", "没有对应的待送达消息");
        sender_it->second->send(protocol::MessageType::error, error_body);
        return;
    }
    // A 的会话，用于通知“已送达”
    const auto original_sender_it = m_sessions.find(delivery_it->second.sender_session_id);
    if (original_sender_it == m_sessions.end() ||
        !isCurrentAuthenticatedSession(delivery_it->second.sender_session_id, delivery_it->second.sender_username))
    {
        m_pendingDeliveries.erase(delivery_it);
        return;
    }

    boost::json::object delivered;
    delivered["local_id"] = delivery_it->second.sender_local_id;
    delivered["status"] = "delivered";
    original_sender_it->second->send(protocol::MessageType::delivery_receipt, boost::json::serialize(delivered));

    // 已处理的 message_id 不再接受第二次回执，重复回执会在 find() 时被拒绝。
    m_pendingDeliveries.erase(delivery_it);
}

// 功能：记录待投递消息，避免重复投递。
bool Server::rememberPendingDelivery(const std::string &message_id, const std::string &sender_username,
                                     SessionId sender_session_id, const std::string &sender_local_id,
                                     const std::string &recipient_username)
{
    if (message_id.empty() || sender_username.empty() || sender_session_id == 0 || sender_local_id.empty() ||
        recipient_username.empty())
    {
        return false;
    }
    const auto [it, inserted] = m_pendingDeliveries.emplace(
        message_id, PendingDelivery{sender_username, sender_session_id, sender_local_id, recipient_username});

    // inserted == true：该 message_id 首次登记成功。
    // inserted == false：理论上只可能是极小概率的 message_id 冲突，不能覆盖原记录。
    return inserted;
}

// 功能：删除与断开连接会话相关的所有待投递消息。
void Server::removePendingDeliveriesForSession(SessionId disconnected_session_id,
                                               const std::string &disconnected_username)
{
    for (auto it = m_pendingDeliveries.begin(); it != m_pendingDeliveries.end();)
    {
        const bool recipient_disconnected =
            !disconnected_username.empty() && it->second.recipient_username == disconnected_username;
        const bool sender_disconnected = it->second.sender_session_id == disconnected_session_id;

        if (recipient_disconnected || sender_disconnected)
        {
            it = m_pendingDeliveries.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void Server::handleHistoryQuery(SessionId sender_id, const protocol::Message &message)
{
    const auto payload_result = protocol::parseHistoryQueryPayload(message.body);
    const auto session_it = m_sessions.find(sender_id);

    if (session_it == m_sessions.end())
    {
        return;
    }
    if (payload_result.error != protocol::HistoryQueryPayloadError::none)
    {
        session_it->second->send(protocol::MessageType::error,
                                 makeHistoryError("history_validation_failed", "历史查询校验失败"));
        return;
    }

    const auto username_it = m_session_to_username.find(sender_id);
    if (username_it == m_session_to_username.end())
    {
        session_it->second->send(protocol::MessageType::error,
                                 makeHistoryError("sender_not_registered", "会话尚未登记认证身份"));
        return;
    }

    if (!isCurrentAuthenticatedSession(sender_id, username_it->second))
    {
        session_it->second->send(protocol::MessageType::error,
                                 makeHistoryError("session_replaced", "当前登录已在其他连接接管"));
        return;
    }
    // 接着做 protocol 游标 → repository 游标的转换。
    std::optional<repository::HistoryCursor> before;
    if (payload_result.before.has_value())
    {
        before =
            repository::HistoryCursor{payload_result.before->server_received_at_ms, payload_result.before->message_id};
    }
    repository::HistoryQueryResult query_result;
    if (!m_database_available ||
        !m_message_repository->loadRecentForUser(username_it->second, before, payload_result.limit, query_result))
    {
        session_it->second->send(protocol::MessageType::error,
                                 makeHistoryError("database_read_failed", "历史消息读取失败"));
        return;
    }

    // 1. 先准备两个容器
    std::vector<boost::json::array> chunks; // 已经确定不会超长的完整块
    boost::json::array current_chunk;       // 当前还在尝试塞消息的块

    // 2. 每条 StoredMessage 先转成 JSON 对象
    for (const auto &stored_message : query_result.messages)
    {
        const auto message_object = makeHistoryMessageObject(stored_message);

        // 3. 不直接修改当前块，先复制出候选块
        // 还不知道加入后会不会超过 2048。
        // 先试算，失败时可以直接丢弃 candidate，原来的 current_chunk 保持不变
        auto candidate = current_chunk;
        candidate.emplace_back(message_object);

        // 4. 用完整“最后块格式”试算实际 body 大小
        // 最后块字段最多，包含 has_more 和 next_cursor。如果这个较大的版本也能装下，普通块肯定能装下
        const auto candidate_body = makeHistoryResultBody(payload_result.request_id, candidate, true, query_result);
        if (candidate_body.size() <= protocol::kMaxFrameBodyLength)
        {
            current_chunk = std::move(candidate);
            continue;
        }

        // 5. 处理放不下的情况
        //  current_chunk 本来为空：说明单条记录也超长，返回错误并停止
        if (current_chunk.empty())
        {
            std::cerr << "单条历史记录超过响应帧大小限制" << std::endl;
            session_it->second->send(protocol::MessageType::error,
                                     makeHistoryError("history_message_too_large", "单条历史记录超过响应帧大小限制"));
            return;
        }
        // 若不为空，说明只是“m3 加到 m1、m2 后太大”，先把旧块保存
        chunks.push_back(std::move(current_chunk));
        // 接着单独验证 m3
        boost::json::array single_message_chunk;
        single_message_chunk.emplace_back(message_object);

        const auto single_message_body =
            makeHistoryResultBody(payload_result.request_id, single_message_chunk, true, query_result);
        if (single_message_body.size() > protocol::kMaxFrameBodyLength)
        {
            std::cerr << "单条历史记录超过响应帧大小限制" << std::endl;
            session_it->second->send(protocol::MessageType::error,
                                     makeHistoryError("history_message_too_large", "单条历史消息无法装入响应帧"));
            return;
        }
        // 不超长：让它成为新的当前块
        current_chunk = std::move(single_message_chunk);
    }
    // 6. 循环结束后收尾
    if (current_chunk.empty() && chunks.empty())
    {
        chunks.emplace_back(); // 空查询仍要回一个空 messages 的最终响应
    }
    else if (!current_chunk.empty())
    {
        chunks.push_back(std::move(current_chunk));
    }
    // 7. 所有块确定后，才发送
    std::vector<std::string> response_bodies;
    response_bodies.reserve(chunks.size());

    for (std::size_t index = 0; index < chunks.size(); ++index)
    {
        const bool is_last_chunk = index + 1 == chunks.size();

        auto body = makeHistoryResultBody(payload_result.request_id, chunks[index], is_last_chunk, query_result);

        response_bodies.push_back(std::move(body));
    }
    session_it->second->sendHistoryResultBodies(std::move(response_bodies));
}

// 功能：构建在线用户列表帧的 body 部分。
std::optional<std::string> Server::buildOnlineUsersBody(std::vector<std::string> usernames)
{
    // 1.排序
    std::sort(usernames.begin(), usernames.end());
    // 2.去重
    usernames.erase(std::unique(usernames.begin(), usernames.end()), usernames.end());
    // 3.检查人数上限
    if (usernames.size() > protocol::kMaxOnlineUsersSnapshotCount)
    {
        return std::nullopt;
    }
    // 4.检验每个用户名
    for (const auto &username : usernames)
    {
        if (!protocol::isValidUsername(username))
        {
            return std::nullopt;
        }
    }
    // 5.构造JSON
    boost::json::array users;
    users.reserve(usernames.size());

    for (const auto &username : usernames)
    {
        users.emplace_back(username);
    }
    boost::json::object root;
    root["users"] = std::move(users);
    std::string body = boost::json::serialize(root);

    // 检查实际序列化后的帧体长度
    if (body.size() > protocol::kMaxFrameBodyLength)
    {
        return std::nullopt;
    }

    return body;
}

void Server::handleAuthenticationRequest(SessionId session_id, std::string username)
{
    const auto session_it = m_sessions.find(session_id);
    // 1. 找候选 Session 是否仍存在
    if (session_it == m_sessions.end())
    {
        return;
    }
    // 2. 局部复制用户名列表
    std::vector<std::string> candidate_usernames;
    candidate_usernames.reserve(m_username_to_session.size() + 1);

    for (const auto &[online_username, ignored_session_id] : m_username_to_session)
    {
        // 3. 加候选用户名
        candidate_usernames.push_back(online_username);
    }
    candidate_usernames.push_back(username);
    // 4.构造并验证 candidate_body
    auto candidate_body = buildOnlineUsersBody(std::move(candidate_usernames));
    // 5. 失败则 rejectAuthentication 后 return
    if (!candidate_body.has_value())
    {
        session_it->second->rejectAuthentication(makeOnlineUsersCapacityErrorBody(),
                                                 "online_snapshot_capacity_exceeded");
        return;
    }
    // 只从这里开始，才允许改真实在线映射。
    // 6. 记录旧同名 SessionId（如有）
    std::optional<SessionId> old_session_id;
    if (const auto old_it = m_username_to_session.find(username);
        old_it != m_username_to_session.end() && old_it->second != session_id)
    {
        old_session_id = old_it->second;
    }
    // 7. 提交 username → session_id
    // 8. 提交 session_id → username
    m_username_to_session.insert_or_assign(username, session_id);
    m_session_to_username.insert_or_assign(session_id, username);
    // 9. 新 Session 入队 auth.ok → online_users
    session_it->second->completeAuthentication(std::move(username), *candidate_body);
    // 10. 向既有 Session 发送同一份 body
    sendOnlineUsersBody(*candidate_body, session_id);
    // 11. 请求旧同名 Session 关闭
    if (old_session_id.has_value())
    {
        if (const auto old_session_it = m_sessions.find(*old_session_id); old_session_it != m_sessions.end())
        {
            old_session_it->second->requestClose();
        }
    }
}

void Server::handleAuthenticationTimeout(const SessionId session_id)
{
    const auto session_it = m_sessions.find(session_id);
    if (session_it == m_sessions.end())
    {
        return;
    }
    if (m_session_to_username.find(session_id) != m_session_to_username.end())
    {
        return;
    }
    session_it->second->rejectAuthentication(makeAuthenticationTimeoutErrorBody(), "authentication_timeout");
}

void Server::sendOnlineUsersBody(const std::string &online_users_body, std::optional<SessionId> excluded_session_id)
{
    for (const auto &[username, session_id] : m_username_to_session)
    {
        if (excluded_session_id.has_value() && session_id == *excluded_session_id)
        {
            continue;
        }

        if (const auto session_it = m_sessions.find(session_id); session_it != m_sessions.end())
        {
            session_it->second->send(protocol::MessageType::online_users, online_users_body);
        }
    }
}

// 功能：广播在线用户列表。
void Server::broadcastOnlineUsers()
{
    std::vector<std::string> usernames;
    for (const auto &it : m_username_to_session)
    {
        usernames.emplace_back(it.first);
    }
    const auto body = buildOnlineUsersBody(usernames);
    if (!body.has_value())
    {
        std::cerr << "无正文信息" << std::endl;
        return;
    }

    sendOnlineUsersBody(*body);
}

// 功能：检查会话是否当前用户会话。
bool Server::isCurrentAuthenticatedSession(const SessionId session_id, const std::string &username) const
{
    const auto it = m_username_to_session.find(username);
    return it != m_username_to_session.end() && it->second == session_id;
}
} // namespace net
