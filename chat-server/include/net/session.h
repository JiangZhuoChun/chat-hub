#pragma once

#include "protocol/frame_decoder.h"
#include "auth/auth_introspection_client.h"

#include <asio.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace net
{

class Session;

// ==================== 模块：会话基础类型与回调 ====================
// 功能：保存等待写入 Socket 的完整协议帧及其消息类型。
struct WriteItem
{
    // 功能：记录该帧的业务类型，便于队列调试和协议观察。
    protocol::MessageType type;
    // 功能：保存已完成编码、等待异步写出的完整协议帧。
    std::string frame;
};

// 功能：定义服务端为每个 TCP 连接分配的唯一会话标识。
using SessionId = uint32_t;

// 功能：定义由 Server 和异步回调共同持有的会话智能指针类型。
using SessionPtr = std::shared_ptr<Session>;

// 功能：定义会话将完整业务消息交给 Server 路由时使用的回调类型。
using MessageCallback = std::function<void(SessionId, protocol::Message)>;

// 功能：定义会话断开后通知 Server 清理在线表时使用的回调类型。
using DisconnectCallback = std::function<void(const SessionId)>;

// 功能：定义会话请求认证时通知 Server 的回调类型。
using AuthenticationRequestedCallback = std::function<void(SessionId, const std::string &)>;

// 功能：通知 Server 认证截止已到；由 Server strand 裁决超时拒绝或已准入忽略。
using AuthenticationTimeoutCallback = std::function<void(SessionId)>;

// ==================== 模块：单个 TCP 会话 ====================
class Session : public std::enable_shared_from_this<Session>
{
  public:
    // ==================== 模块：生命周期与对外发送 ====================
    // 功能：接管已接受的 Socket，并保存消息、断开和认证准入请求回调。
    Session(asio::ip::tcp::socket socket, SessionId session_id,
            std::chrono::milliseconds authentication_timeout,
            std::shared_ptr<auth::IAuthIntrospectionClient> auth_introspection_client,
            MessageCallback on_message, DisconnectCallback on_disconnect,
            AuthenticationRequestedCallback on_authentication_requested,
            AuthenticationTimeoutCallback on_authentication_timeout
            );

    // 功能：将首个异步读取任务投递到本会话 strand，开始处理客户端字节流。
    void start();

    // 功能：将待发送消息投递到本会话 strand，避免多个线程并发写同一 Socket。
    void send(protocol::MessageType type, std::string body);

    // 功能：按实际写入完成顺序发送多个已校验的历史响应正文，避免一次性挤满通用写队列。
    // 输入：每个 body 都是完整 JSON，且长度已经不超过 kMaxFrameBodyLength。
    // 输出：每个 body 被编码为一个 history_result 帧，按原顺序发送。
    void sendHistoryResultBodies(std::vector<std::string> bodies);

    // 功能：Server 可调用；它只把关闭任务 asio::post 到 m_strand
    void requestClose();

    // 功能：仅由 Server 在在线准入成功后调用；提交已确认的认证状态，
    //       再按 auth.ok、online_users 的顺序将两帧加入写队列。
    void completeAuthentication(std::string username, std::string online_users_body);

    // 功能：仅由 Server 在在线准入拒绝后调用；先发送已序列化的错误帧，
    //       再在当前写队列排空后关闭连接，避免客户端收不到错误原因。
    void rejectAuthentication(std::string error_body, std::string error_code);

  private:
    void startAuthenticationDeadlineOnStrand();

    void cancelAuthenticationDeadlineOnStrand();

    void handleAuthenticationDeadlineOnStrand(const std::error_code &error);

    void handleIntrospectionResultOnStrand(auth::IntrospectionResult result);

    // ==================== 模块：异步读取与帧解码 ====================
    // 功能：异步读取 Socket 字节，交给帧解码器处理后继续安排下一次读取。
    // 失败：读取或协议解码失败时关闭会话并通知 Server 清理在线记录。
    void doRead();

    // ==================== 模块：串行写队列 ====================
    // 功能：校验正文长度、编码帧并压入写队列，必要时启动首个异步写操作。
    void enqueueAndWrite(protocol::MessageType type, const std::string &body, bool allow_terminal_overflow = false);

    // 功能：写出队首帧，完成后移除队首并继续写剩余帧；若队列排空且标记关闭，
    //       则在最后一帧写完后关闭连接。
    void writeFrame();

    // 功能：仅在通用写队列为空时，将下一块历史正文交给通用写队列发送。
    void startNextHistoryResultBody();


    // 功能：构造包含 local_id 的聊天错误 JSON，供客户端定位失败气泡。
    static std::string makeChatError(const std::string &local_id, const std::string &code, const std::string &message);

    static std::string makeAuthError(const std::string &code, const std::string &message);

    // 功能：根据认证状态和消息类型处理认证、聊天、心跳及错误帧。
    void handlerMessage(const protocol::Message &message);

    //
    static std::string makeHistoryError(const std::string &code, const std::string &message);

    // 功能
    void rejectAuthenticationOnStrand(const std::string &error_body, const std::string &error_code);

    // ==================== 模块：关闭与日志 ====================
    // 功能：幂等地标记会话断开，并通知 Server 从在线表中移除当前会话。
    void closeOnStrand();

    // 功能：统一输出会话相关事件，便于排查连接和协议问题。
    void log(std::string_view phase, std::string_view event, std::string_view code,
             std::optional<std::size_t> actual = std::nullopt, std::optional<std::size_t> limit = std::nullopt) const;

    // ==================== 模块：写队列限制 ====================
    // 功能：限制慢客户端积压的待发送帧数量，超过限制后关闭会话。
    static constexpr std::size_t kMaxWriteQueueSize = 3;

    // ==================== 模块：网络与接收资源 ====================
    // 功能：保存当前客户端连接的 TCP 套接字。
    asio::ip::tcp::socket m_socket;

    // 功能：保证当前会话的读写、状态修改和队列操作串行执行。
    asio::strand<asio::any_io_executor> m_strand;

    asio::steady_timer m_authentication_timer;

    const std::chrono::milliseconds m_authentication_timeout;

    std::shared_ptr<auth::IAuthIntrospectionClient> m_auth_introspection_client;
    //当前 Session 正在等待的那一次 introspection
    std::shared_ptr<auth::IAuthIntrospectionRequest> m_auth_introspection_request;

    // 功能：缓存半包和粘包并还原完整协议帧。
    protocol::FrameDecoder m_decoder;

    // 功能：保存每次异步读取到的原始字节。
    std::array<char, protocol::kMaxFrameBodyLength> m_read_buffer{};

    // ==================== 模块：待发送帧队列 ====================
    // 功能：按顺序保存等待异步写出的完整协议帧。
    std::deque<WriteItem> m_write_queue;

    // 功能：保存尚未实际写出的历史响应正文；队首永远是下一块。
    std::deque<std::string> m_pending_history_result_bodies;

    // 功能：标记当前写队列排空后应关闭连接；用于认证准入拒绝的 error 帧收尾。
    bool m_close_after_write{false};

    // ==================== 模块：会话生命周期状态 ====================
    // 功能：防止同一会话重复触发断开回调。
    bool m_disconnected{false};

    // ==================== 模块：Server 协作回调与会话标识 ====================
    // 功能：保存将已校验业务消息交给 Server 的回调。
    MessageCallback m_on_message;

    // 功能：保存本会话在 Server 中的唯一标识。
    SessionId m_id;

    // 功能：保存会话关闭时通知 Server 的回调。
    DisconnectCallback m_on_disconnect;

    // 功能：JWT 与用户名合同通过后，请求 Server 决定是否允许进入在线用户集合。
    AuthenticationRequestedCallback on_authentication_requested;

    // 功能：将 Session strand 的认证截止事件投递回 Server strand 统一裁决。
    AuthenticationTimeoutCallback m_on_authentication_timeout;

    // ==================== 模块：认证身份状态 ====================
    // 功能：记录当前 Socket 是否已通过令牌认证。
    bool m_authenticated{false};
    // 功能：JWT 和用户名已验证，但 Server 尚未确认在线准入。
    bool m_authentication_pending{false};

    // 功能：保存认证成功后从令牌中提取的用户名。
    std::string m_username;
};

} // namespace net
