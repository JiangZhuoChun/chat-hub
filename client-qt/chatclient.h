#pragma once
#include "chat_types.h"
#include <QAbstractSocket>
#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>
#include <chrono>
class ChatClient : public QObject
{
    Q_OBJECT

  public:
    // ==================== 模块：生命周期与连接认证接口 ====================
    // 功能：创建聊天 TCP 客户端，初始化套接字、连接计时器和信号槽关系。
    explicit ChatClient(QObject *parent = nullptr);

    // 功能：使用登录服务返回的令牌连接 chat-server，并开始等待认证结果。
    // 失败：令牌为空或 TCP 连接失败时，通过认证或连接失败信号通知调用方。
    void connectWithToken(const QString &token);

    // 功能：停止连接计时器，并请求以正常断开方式关闭聊天服务器连接。
    void disconnectFromServer();

    // 功能：返回当前客户端是否已经通过聊天服务器认证。
    bool isAuthenticated() const;

    // ==================== 模块：聊天发送接口 ====================
    // 功能：构造聊天 JSON 正文并发送，使用 local_id 关联确认、错误和重试状态。
    // 失败：未认证、参数为空或帧写入失败时，通过 chatSendFailed() 通知调用方。
    void sendChatMessage(ChatMessage message);

    // 功能：为接收完成本地处理的消息发送回执；回执按服务端 message_id 关联。
    void sendDeliveryReceipt(const QString &message_id);

    // 功能：返回当前连接最近一次收到且通过校验的在线用户快照。
    QStringList onlineUsers() const;

  signals:
    // ==================== 模块：认证与连接结果通知 ====================
    // 功能：通知认证帧已经写入客户端发送缓冲区。
    void authFrameSent();

    // 功能：通知服务端已通过本客户端的令牌认证。
    void authSucceeded();

    // 功能：通知认证阶段收到拒绝、协议错误或服务端提前断开的原因。
    void authFailed(const QString &reason);

    // 功能：通知尚未建立 TCP 连接时发生的连接错误或连接超时。
    void connectionFailed(const QString &reason);

    // 功能：通知已经认证的连接随后断开。
    void disconnected();

    void onlineUsersChanged(const QStringList &users);

    // ==================== 模块：聊天业务结果通知 ====================
    // 功能：通知聊天消息已经写入客户端发送缓冲区，并提供用于显示的发送时间。
    void chatMessageQueued(const ChatMessage &message);

    // 功能：通知服务端已接受 local_id 对应的聊天消息。
    void chatMessageAccepted(const ChatMessage &update);

    void chatMessageDelivered(const ChatMessage &update);

    // 功能：通知收到服务端转发的聊天消息及其发送者、接收者和时间。
    void chatMessageReceived(const ChatMessage &message);

    // 功能：通知 local_id 对应的聊天消息发送或服务端处理失败。
    void chatSendFailed(const ChatMessage &update);

    // 功能：通知不属于某条聊天消息的普通服务端错误。
    void serverError(const QString &reason);

    // 功能：当前历史请求的全部合法分块收齐后，通知上层获得完整首屏及是否还有更早消息。
    // 边界：仅最终块发射；本类不在此处合并 UI，也不为历史消息发送送达回执。
    void historyPageReceived(const QList<ChatMessage> &messages, bool has_more);

  private slots:
    // ==================== 模块：Socket 事件处理 ====================
    // 功能：TCP 连接建立后停止计时并发送认证帧。
    void onSocketConnected();

    // 功能：读取套接字当前可用字节，并尝试解析全部完整协议帧。
    void onSocketReady();

    // 功能：按当前认证状态将套接字错误转换为连接、认证或断开通知。
    void onSocketError(QAbstractSocket::SocketError socket_error);

    // 功能：按断开前的认证状态通知认证失败、连接失败或正常连接中断。
    void onSocketDisconnected();

  private:
    // ==================== 模块：认证状态机 ====================
    // 功能：记录客户端从未连接、连接中、等待认证到认证完成的状态。
    enum class AuthState
    {
        idle,
        connecting,
        waitingAuthResult,
        authenticated
    };

    // 功能：限制首次历史查询及其响应累计的最大消息条数。
    static constexpr int kInitialHistoryLimit = 50;

    // 只限制 waitingAuthResult 阶段，不能用于 TCP 建连阶段
    static constexpr auto kAuthenticationTimeout = std::chrono::milliseconds{5000};

    // ==================== 模块：初始化与连接辅助 ====================
    // 功能：绑定 Socket、连接计时器的 Qt 信号与本类事件处理函数。
    void connectSlots();

    // 功能：以一致顺序结束认证：先停止计时器和清理旧会话，再中止连接，最后通知界面。
    void failAuthentication(const QString &reason);

    // 功能：将接收到的 JSON 聊天消息转换为 ChatMessage 对象。
    static ChatMessage makeReceivedChatMessage(const QJsonObject &object);
    // 功能：将按 local_id 定位的本地状态更新转换为 ChatMessage；accepted 时额外携带服务器分配的 message_id。
    static ChatMessage makeMessageStateUpdate(const QString &local_id, ChatMessageStatus status,
                                              const QString &failure_reason = {}, const QString &message_id = {},
                                              std::optional<qint64> server_received_at_ms = std::nullopt);

    // ==================== 模块：协议帧编码与发送 ====================
    // 功能：将 type 和正文编码为 [魔数][版本][类型][长度][正文] 格式的字节帧。
    static QByteArray makeFrame(quint8 type, const QByteArray &body);

    // 功能：校验连接和正文长度后将完整帧写入 TCP 发送缓冲区。
    // 失败：正文超长、TCP 未连接或写入失败时返回 false 并写入错误原因。
    bool writeFrame(quint8 type, const QByteArray &body, QString &error);

    // 功能：将保存的令牌作为 type=5 正文发送给聊天服务器。
    void sendAuthFrame();

    // 功能：认证完成后发送 type=2 心跳请求，验证连接是否仍可写入。
    void sendPing();

    // 功能：认证成功后生成请求 ID，发送首屏 50 条历史查询。
    // 返回：写入 TCP 发送缓冲区成功返回 true；失败时清理历史状态、报告错误并中止连接。
    bool sendInitialHistoryQuery();

    // ==================== 模块：收帧与类型分派 ====================
    // 功能：从接收缓存中解析完整帧，处理半包与粘包，并按 type 分派正文。
    // 失败：协议头非法时断开连接并通过认证失败信号通知界面。
    void processReceivedFrames();

    // 功能：根据协议 type 将正文交给认证、聊天、错误、心跳或确认处理函数。
    void dispatchFrame(quint8 type, const QByteArray &body);

    // ==================== 模块：各类型正文处理 ====================
    // 功能：解析认证响应正文，并更新认证状态或通知认证失败。
    void handleAuthBody(const QByteArray &body);

    // 功能：解析转发的聊天正文，并通知界面显示收到的消息。
    void handleChatBody(const QByteArray &body);

    // 功能：解析服务端错误正文，并按 local_id 和认证状态发出对应错误通知。
    void handleErrorBody(const QByteArray &body);

    // 功能：收到心跳请求后发送同一正文的心跳响应。
    void handlePingBody(const QByteArray &body);

    // 功能：接收心跳响应；当前协议不需要额外状态更新。
    static void handlePongBody(const QByteArray &body);

    // 功能：解析聊天确认正文，并通知界面更新 local_id 对应消息状态。
    void handleChatAckBody(const QByteArray &body);

    // 功能：解析最终送达回执，并通知界面更新已有消息的 Delivered 状态。
    void handleDeliveryReceiptBody(const QByteArray &body);

    // 功能：严格校验在线用户快照，更新缓存后通知已创建的界面。
    void handleOnlineUsersBody(const QByteArray &body);

    // 功能：校验匹配当前 request_id 的历史分块，暂存消息，并仅在最终块通知上层。
    // 失败：正文、消息、分页字段或数量上限非法时丢弃本次半页历史并报告错误。
    void handleHistoryResultBody(const QByteArray &body);

    // 功能：清除已失效的在线快照，并通知已创建的界面同步清空。
    void clearOnlineUsers();
    // 功能：清空当前连接中尚未完成的历史请求 ID 与分块暂存消息
    void clearPendingHistoryRequest();

    // ==================== 模块：网络资源 ====================
    // 功能：维护与 chat-server 的 TCP 连接，并产生连接与读写事件。
    QTcpSocket m_socket;
    // 功能：限制 TCP 建连阶段的最长等待时间。
    QTimer m_connect_timer;

    // ==================== 模块：会话与接收缓存状态 ====================
    // 功能：保存本次认证要使用的令牌。
    QString m_token;

    // 功能：缓存 TCP 半包或尚未解析完成的粘包数据。
    QByteArray m_received_buffer;

    // 功能：保存当前连接和认证阶段，供事件处理函数决定错误通知类型。
    AuthState m_state{AuthState::idle};

    // 功能：限制认证阶段的最长等待时间。
    QTimer m_auth_timer;

    // 功能：缓存最后一份有效在线用户快照，供晚创建的主窗口主动回填。
    QStringList m_online_users;

    // 功能：保存当前连接仍在等待响应的历史查询 ID。
    QString m_active_history_request_id;
    // 功能：暂存已经校验通过、但尚未收到最终块的一页历史消息。
    QList<ChatMessage> m_pending_history_messages;
};
