#pragma once

#include "auth/auth_introspection_client.h"
#include "net/session.h"
#include "repository/message_repository_contract.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
namespace net
{

// ==================== 模块：聊天服务器 ====================
class Server
{
  public:
    // ==================== 模块：生命周期与监听 ====================
    // 功能：创建 Server strand、绑定监听端口，并准备下一次接受连接的 Socket。
    Server(asio::io_context &io_context,
            std::uint16_t port,
            std::string database_path,
            std::chrono::milliseconds authentication_timeout,
            auth::AuthIntrospectionConfig auth_introspection_config,
            std::unique_ptr<repository::IMessageRepository> message_repository,
            std::shared_ptr<auth::IAuthIntrospectionClient>
                auth_introspection_client = nullptr);

    // 功能：输出监听信息并开始持续异步接受新的 TCP 连接。
    void start();

  private:
    // 功能：保存一条等待接收者回执的消息的原发送者归属。
    struct PendingDelivery
    {
        // 功能：标识原消息的认证发送者，供日志与后续业务扩展使用。
        std::string sender_username;
        // 功能：定位原发送者当前会话，用于投递最终送达状态。
        SessionId sender_session_id;
        // 功能：保存 A 侧的本地消息标识，使最终状态通知仍能定位 A 的既有气泡。
        std::string sender_local_id;
        // 功能：保存应发送回执的接收者身份，防止其他已认证用户伪造回执。
        std::string recipient_username;
    };

    // 功能：以服务端生成且全局唯一的 message_id 索引每一条待送达记录。
    using PendingDeliveryMap = std::unordered_map<std::string, PendingDelivery>;

    // 功能：接受一个连接、创建 Session、登记在线表，并继续等待下一次连接。
    void doAccept();

    // ==================== 模块：会话登记与清理 ====================
    // 功能：将新建 Session 放入会话表，使它可被后续路由操作找到。
    void addSession(SessionId session_id, const SessionPtr &session);

    // 功能：移除断开会话及其关联用户名，防止继续向失效连接路由消息。
    void removeSession(SessionId session_id);

    // ==================== 模块：聊天消息路由 ====================
    // 功能：接收 Session 上交的完整消息，并交给私聊路由函数处理。
    void onSessionMessage(SessionId sender_id, const protocol::Message &message);

    // 功能：校验发送者和接收者在线状态，向接收者转发消息并回复发送确认。
    void sendToUser(SessionId sender_id, const protocol::Message &message);

    // 功能：校验接收者的送达回执，并将最终送达状态转发给原发送者。
    void handleDeliveryReceipt(SessionId receipt_sender_id, const protocol::Message &message);

    // 功能：以 message_id 登记待送达消息，供接收者回执后反查发送方会话和 A 侧 local_id。
    bool rememberPendingDelivery(const std::string &message_id, const std::string &sender_username,
                                 SessionId sender_session_id, const std::string &sender_local_id,
                                 const std::string &recipient_username);

    // 功能：删除断开会话作为发送者或真正离线接收者时遗留的待送达记录。
    void removePendingDeliveriesForSession(SessionId disconnected_session_id, const std::string &disconnected_username);

    void handleHistoryQuery(SessionId sender_id, const protocol::Message &message);

    // 功能：从认证用户名映射生成稳定排序的 online_users JSON 正文。
    static std::optional<std::string> buildOnlineUsersBody(std::vector<std::string> usernames);

    // 完成候选计算、失败拒绝、成功提交和同名接管
    void handleAuthenticationRequest(SessionId session_id, std::string username);

    void handleAuthenticationTimeout(SessionId session_id);

    // 只发送一份已经验证过的 JSON body，不再排序、不再序列化、不再做业务判断。
    void sendOnlineUsersBody(const std::string &online_users_body,
                             std::optional<SessionId> excluded_session_id = std::nullopt);

    // 功能：向全部已认证且仍有效的会话推送完整在线用户快照。
    void broadcastOnlineUsers();

    // 功能：判断会话是否仍是指定用户名当前有效的认证会话。
    bool isCurrentAuthenticatedSession(SessionId session_id, const std::string &username) const;

    // ==================== 模块：并发执行资源 ====================
    // 功能：保证在线会话表和用户名映射只在 Server 的串行执行器中访问。
    asio::strand<asio::any_io_executor> m_strand;

    // ==================== 模块：监听资源 ====================
    // 功能：监听聊天服务器端口并异步接受新连接。
    asio::ip::tcp::acceptor m_acceptor;

    // 功能：在异步接受期间保存待转交给新 Session 的 Socket。
    asio::ip::tcp::socket m_pending_socket;
    // 功能：保存本次实际打开的 SQLite 路径，供启动日志说明有效运行配置
    const std::string m_database_path;
    // 功能：保存本次实际打开的认证超时时间，供启动日志说明有效运行配置
    const std::chrono::milliseconds m_authentication_timeout;

    // ==================== 模块：会话表 ====================
    // 功能：为下一个连接分配递增且唯一的会话标识。
    SessionId m_next_session_id{1};

    // 功能：按会话标识保存所有仍由 Server 管理的连接。
    std::unordered_map<SessionId, SessionPtr> m_sessions;

    // ==================== 模块：用户路由表 ====================
    // 功能：将认证用户名映射到当前在线会话标识。
    std::unordered_map<std::string, SessionId> m_username_to_session;

    // 功能：将会话标识反向映射到认证用户名，供断开清理使用。
    std::unordered_map<SessionId, std::string> m_session_to_username;

    // 功能：保存尚未被接收者确认的消息，生命周期仅限当前服务进程。
    PendingDeliveryMap m_pendingDeliveries;

    auth::AuthIntrospectionConfig m_auth_introspection_config;

    std::shared_ptr<auth::IAuthIntrospectionClient>m_auth_introspection_client;

    std::unique_ptr<repository::IMessageRepository> m_message_repository;

    // 功能：记录 SQLite 是否已完成打开和初始化；不可用时拒绝依赖持久化的聊天写入。
    bool m_database_available{false};
};

} // namespace net
