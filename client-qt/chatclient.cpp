#include "chatclient.h"

#include "protocol/chat_protocol.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QUuid>

// ==================== 模块：生命周期 ====================
// 功能：创建套接字和连接计时器，并完成所有内部信号槽连接。
ChatClient::ChatClient(QObject *parent) : QObject(parent), m_socket(this), m_connect_timer(this), m_auth_timer(this)
{
    m_connect_timer.setSingleShot(true);
    m_auth_timer.setSingleShot(true);
    connectSlots();
}

// ==================== 模块：连接与认证对外接口 ====================
// 功能：保存令牌、重置接收缓存，并连接本机 9000 端口的聊天服务器。
// 失败：令牌为空时直接通知认证失败；连接失败或超时由 Socket 和计时器事件通知。
void ChatClient::connectWithToken(const QString &token)
{
    if (token.isEmpty())
    {
        emit authFailed("登录响应中没有 token");
        return;
    }

    m_auth_timer.stop();
    clearPendingHistoryRequest();

    if (m_socket.state() != QAbstractSocket::UnconnectedState)
    {
        m_state = AuthState::idle;
        m_socket.abort();
    }

    m_token = token;
    m_received_buffer.clear();
    clearOnlineUsers();
    m_state = AuthState::connecting;
    m_connect_timer.start(5000);

    m_socket.connectToHost(QHostAddress::LocalHost, 9000);
}

// 功能：停止连接超时检查，并请求 Socket 在已发送数据处理完成后正常断开。
void ChatClient::disconnectFromServer()
{
    m_connect_timer.stop();
    m_auth_timer.stop();
    clearPendingHistoryRequest();
    m_state = AuthState::idle;
    clearOnlineUsers();
    m_socket.disconnectFromHost();
}

// 功能：根据认证状态机返回当前连接是否已通过服务器认证。
bool ChatClient::isAuthenticated() const
{
    return m_state == AuthState::authenticated;
}

// ==================== 模块：聊天发送对外接口 ====================
// 功能：创建包含接收者、正文、local_id 和协调世界时发送时间的聊天帧并写入套接字。
// 失败：未认证、必要字段为空或写入失败时通知 local_id 对应的聊天消息失败。
void ChatClient::sendChatMessage(ChatMessage message)
{
    const QString normalized_to = message.to.trimmed();

    if (message.local_id.isEmpty() || !message.send_at.isValid())
    {
        emit serverError(QStringLiteral("发送消息缺少本地标识或时间"));
        return;
    }

    if (m_state != AuthState::authenticated)
    {
        emit chatSendFailed(makeMessageStateUpdate(message.local_id, ChatMessageStatus::Failed, "未认证"));
        return;
    }
    if (normalized_to.isEmpty() || message.content.trimmed().isEmpty())
    {
        emit chatSendFailed(makeMessageStateUpdate(message.local_id, ChatMessageStatus::Failed, "接收者或内容为空"));
        return;
    }
    message.to = normalized_to;

    QJsonObject object;
    object["to"] = normalized_to;
    object["content"] = message.content;
    object["local_id"] = message.local_id;
    object["send_at"] = message.send_at.toUTC().toString(Qt::ISODateWithMs);
    const QByteArray body = QJsonDocument(object).toJson(QJsonDocument::Compact);

    QString error;
    if (!writeFrame(static_cast<quint8>(protocol::MessageType::chat), body, error))
    {
        emit chatSendFailed(makeMessageStateUpdate(message.local_id, ChatMessageStatus::Failed, error));
        return;
    }
    emit chatMessageQueued(message);
}

// 功能：在接收消息已写入本地模型并完成界面处理后，向服务端发送最终送达回执。
void ChatClient::sendDeliveryReceipt(const QString &message_id)
{
    const QString normalized_message_id = message_id.trimmed();
    if (normalized_message_id.isEmpty())
    {
        emit serverError(QStringLiteral("送达回执缺少 message_id"));
        return;
    }
    if (m_state != AuthState::authenticated)
    {
        emit serverError(QStringLiteral("未认证，无法发送送达回执"));
        return;
    }

    QJsonObject object;
    object["message_id"] = normalized_message_id;
    const auto body = QJsonDocument(object).toJson(QJsonDocument::Compact);

    QString error;
    if (!writeFrame(static_cast<quint8>(protocol::MessageType::delivery_receipt), body, error))
    {
        emit serverError(QStringLiteral("送达回执发送失败：") + error);
    }
}

// 功能：返回最后一份有效在线用户快照，供晚创建的主窗口同步回填。
QStringList ChatClient::onlineUsers() const
{
    return m_online_users;
}

// ==================== 模块：Socket 事件处理 ====================
// 功能：TCP 连接成功后停止连接计时器，并立即发送认证帧。
void ChatClient::onSocketConnected()
{
    m_connect_timer.stop();
    sendAuthFrame();
}

// 功能：读取套接字当前所有可用数据，并尝试从缓存中解析完整帧。
void ChatClient::onSocketReady()
{
    m_received_buffer.append(m_socket.readAll());
    processReceivedFrames();
}

// 功能：根据错误发生前的认证状态向界面发出连接失败、认证失败或断开通知。
void ChatClient::onSocketError(const QAbstractSocket::SocketError socket_error)
{

    m_auth_timer.stop();

    if (socket_error == QAbstractSocket::RemoteHostClosedError)
    {
        return;
    }

    m_connect_timer.stop();
    const AuthState old_state = m_state;
    const QString error_text = m_socket.errorString();

    if (old_state == AuthState::waitingAuthResult)
    {
        failAuthentication(QStringLiteral("认证连接异常：") + error_text);
        return;
    }

    m_state = AuthState::idle;
    clearPendingHistoryRequest();
    clearOnlineUsers();
    m_received_buffer.clear();

    // 先将旧连接的 disconnected 事件在 idle 状态中消耗，再通知界面，避免直接槽中的重连被旧事件误判。
    m_socket.abort();

    if (old_state == AuthState::authenticated)
    {
        emit disconnected();
    }
    else if (old_state == AuthState::connecting)
    {
        emit connectionFailed(error_text);
    }
}

// 功能：根据断开前状态通知认证失败、连接建立前断开或已认证连接断开。
void ChatClient::onSocketDisconnected()
{
    m_connect_timer.stop();
    m_auth_timer.stop();
    const AuthState old_state = m_state;
    m_state = AuthState::idle;
    clearPendingHistoryRequest();
    clearOnlineUsers();

    if (old_state == AuthState::waitingAuthResult)
    {
        emit authFailed("服务器在认证前断开连接");
    }
    else if (old_state == AuthState::authenticated)
    {
        emit disconnected();
    }
    else if (old_state == AuthState::connecting)
    {
        emit connectionFailed("服务端在连接建立前断开了连接");
    }
}

// ==================== 模块：初始化与连接辅助 ====================
// 功能：将套接字和连接超时计时器的事件连接到对应处理函数。
void ChatClient::connectSlots()
{
    connect(&m_socket, &QTcpSocket::connected, this, &ChatClient::onSocketConnected);
    connect(&m_socket, &QTcpSocket::readyRead, this, &ChatClient::onSocketReady);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, &ChatClient::onSocketError);
    connect(&m_socket, &QTcpSocket::disconnected, this, &ChatClient::onSocketDisconnected);

    connect(&m_connect_timer, &QTimer::timeout, this,
            // 功能：仅在连接阶段中止未完成的连接，并通知界面连接超时。
            [this] {
                if (m_state != AuthState::connecting)
                {
                    return;
                }
                clearPendingHistoryRequest();
                m_socket.abort();
                m_state = AuthState::idle;
                emit connectionFailed("连接 chat-server 超时");
            });

    connect(&m_auth_timer, &QTimer::timeout, this, [this] {
        if (m_state != AuthState::waitingAuthResult)
        {
            return;
        }
        failAuthentication(QStringLiteral("等待 chat-server 认证响应超时"));
    });
}

void ChatClient::failAuthentication(const QString &reason)
{
    m_auth_timer.stop();
    m_state = AuthState::idle;
    clearPendingHistoryRequest();
    clearOnlineUsers();
    m_received_buffer.clear();
    m_socket.abort();
    emit authFailed(reason);
}

// 功能：将接收到的 JSON 对象转换为 ChatMessage 对象。
ChatMessage ChatClient::makeReceivedChatMessage(const QJsonObject &object)
{
    ChatMessage message;
    message.message_id = object.value("message_id").toString();
    message.local_id = object.value("local_id").toString();
    message.from = object.value("from").toString();
    message.to = object.value("to").toString();
    message.content = object.value("content").toString();
    message.send_at = QDateTime::fromString(object.value("send_at").toString(), Qt::ISODate);
    message.status = ChatMessageStatus::Received;
    message.server_received_at_ms = object.value(QStringLiteral("server_received_at_ms")).toInteger();

    return message;
}

ChatMessage ChatClient::makeMessageStateUpdate(const QString &local_id, const ChatMessageStatus status,
                                               const QString &failure_reason, const QString &message_id,
                                               std::optional<qint64> server_received_at_ms)
{
    ChatMessage update;
    update.message_id = message_id;
    update.local_id = local_id;
    update.status = status;
    update.failure_reason = failure_reason;
    update.server_received_at_ms = server_received_at_ms;
    update.from = "";
    update.to = "";
    update.content = "";
    update.send_at = QDateTime{};
    return update;
}

// ==================== 模块：协议帧编码与发送 ====================
// 功能：将 type 和正文按大端序编码为聊天服务器使用的完整协议帧。
QByteArray ChatClient::makeFrame(const quint8 type, const QByteArray &body)
{
    QByteArray frame;
    const auto length = static_cast<quint32>(body.size());

    frame.reserve(static_cast<int>(protocol::kFrameHeaderLength) + body.size());
    frame.append(protocol::kFrameMagic >> 8);
    frame.append(protocol::kFrameMagic & 0xFF);
    frame.append(protocol::kProtocolVersion);
    frame.append(static_cast<char>(type));
    frame.append(static_cast<char>(length >> 24 & 0xFF));
    frame.append(static_cast<char>(length >> 16 & 0xFF));
    frame.append(static_cast<char>(length >> 8 & 0xFF));
    frame.append(static_cast<char>(length & 0xFF));
    frame.append(body);

    return frame;
}

// 功能：校验正文长度和连接状态后，将完整协议帧写入 TCP 发送缓冲区。
// 失败：正文超长、套接字未连接或写入失败时返回 false，并写入错误输出参数。
bool ChatClient::writeFrame(const quint8 type, const QByteArray &body, QString &error)
{
    error.clear();
    if (body.size() > static_cast<int>(protocol::kMaxFrameBodyLength))
    {
        error = QStringLiteral("消息体超过协议允许的长度");
        return false;
    }
    if (m_socket.state() != QAbstractSocket::ConnectedState)
    {
        error = QStringLiteral("TCP 尚未连接");
        return false;
    }
    if (m_socket.write(makeFrame(type, body)) == -1)
    {
        error = m_socket.errorString();
        return false;
    }

    return true;
}

// 功能：将保存的令牌作为认证帧正文发送，并转入等待认证响应状态。
// 失败：认证帧无法写入时重置状态并通知认证失败。
void ChatClient::sendAuthFrame()
{
    const QByteArray body = m_token.toUtf8();
    QString error;
    if (!writeFrame(static_cast<quint8>(protocol::MessageType::auth), body, error))
    {
        failAuthentication(QStringLiteral("认证帧发送失败：") + error);
        return;
    }

    m_state = AuthState::waitingAuthResult;
    m_auth_timer.start(kAuthenticationTimeout);
    emit authFrameSent();
}

// 功能：在认证完成后发送心跳请求；写入失败时中止套接字以触发统一断开处理。
void ChatClient::sendPing()
{
    if (m_state != AuthState::authenticated)
    {
        return;
    }

    QString error;
    if (!writeFrame(static_cast<quint8>(protocol::MessageType::ping), {}, error))
    {
        m_socket.abort();
    }
}

bool ChatClient::sendInitialHistoryQuery()
{
    // 开始一个新历史请求前，暂存区一定为空
    clearPendingHistoryRequest();

    const QString request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_active_history_request_id = request_id;

    QJsonObject object;
    object["request_id"] = request_id;
    object["limit"] = kInitialHistoryLimit;

    const auto body = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QString error;
    if (!writeFrame(static_cast<quint8>(protocol::MessageType::history_query), body, error))
    {
        clearPendingHistoryRequest();
        emit serverError(QStringLiteral("初始历史查询发送失败：") + error);
        m_socket.abort();
        return false;
    }
    return true;
}

// ==================== 模块：收帧与类型分派 ====================
// 功能：从接收缓存中循环解析完整帧，支持 TCP 半包和一次收到多帧的情况。
// 失败：帧头不符合协议时清空认证状态、通知认证失败并断开连接。
void ChatClient::processReceivedFrames()
{
    while (m_received_buffer.size() >= static_cast<int>(protocol::kFrameHeaderLength))
    {
        const auto *header = reinterpret_cast<const unsigned char *>(m_received_buffer.constData());
        const auto magic = static_cast<quint16>(header[0] << 8 | header[1]);
        const quint8 version = header[2];
        const quint8 type = header[3];
        // 每个长度字节先转为无符号 32 位数再左移，避免损坏帧触发有符号左移未定义行为。
        const auto body_length = (static_cast<quint32>(header[4]) << 24) | (static_cast<quint32>(header[5]) << 16) |
                                 (static_cast<quint32>(header[6]) << 8) | static_cast<quint32>(header[7]);

        if (magic != protocol::kFrameMagic || version != protocol::kProtocolVersion ||
            !protocol::isKnownMessageType(type) || body_length > protocol::kMaxFrameBodyLength)
        {
            const AuthState old_state = m_state;

            if (old_state == AuthState::waitingAuthResult)
            {
                failAuthentication(QStringLiteral("收到非法聊天协议帧"));
                return;
            }

            m_auth_timer.stop();
            clearPendingHistoryRequest();
            clearOnlineUsers();
            m_received_buffer.clear();
            m_state = AuthState::idle;
            m_socket.abort();

            if (old_state == AuthState::authenticated)
            {
                emit serverError(QStringLiteral("收到非法聊天协议帧"));
                emit disconnected();
            }

            return;
        }

        const int frame_length = static_cast<int>(protocol::kFrameHeaderLength) + static_cast<int>(body_length);
        if (m_received_buffer.size() < frame_length)
        {
            return;
        }

        const QByteArray body =
            m_received_buffer.mid(static_cast<int>(protocol::kFrameHeaderLength), static_cast<int>(body_length));
        m_received_buffer.remove(0, frame_length);
        dispatchFrame(type, body);
    }
}

// 功能：按协议消息类型分派正文，避免将认证和心跳正文错误当作聊天 JSON 解析。
void ChatClient::dispatchFrame(const quint8 type, const QByteArray &body)
{
    if (m_state == AuthState::idle || m_state == AuthState::connecting)
    {
        m_received_buffer.clear();
        return;
    }

    if (m_state == AuthState::waitingAuthResult && type != static_cast<quint8>(protocol::MessageType::auth) &&
        type != static_cast<quint8>(protocol::MessageType::error))
    {
        failAuthentication(QStringLiteral("认证阶段收到非法聊天协议帧"));
        return;
    }

    switch (type)
    {
    case static_cast<quint8>(protocol::MessageType::auth):
        handleAuthBody(body);
        break;
    case static_cast<quint8>(protocol::MessageType::ping):
        handlePingBody(body);
        break;
    case static_cast<quint8>(protocol::MessageType::chat):
        handleChatBody(body);
        break;
    case static_cast<quint8>(protocol::MessageType::error):
        handleErrorBody(body);
        break;
    case static_cast<quint8>(protocol::MessageType::pong):
        handlePongBody(body);
        break;
    case static_cast<quint8>(protocol::MessageType::chat_ack):
        handleChatAckBody(body);
        break;
    case static_cast<quint8>(protocol::MessageType::delivery_receipt):
        handleDeliveryReceiptBody(body);
        break;
    case static_cast<quint8>(protocol::MessageType::online_users):
        handleOnlineUsersBody(body);
        break;
    case static_cast<quint8>(protocol::MessageType::history_result):
        handleHistoryResultBody(body);
        break;
    default:
        emit serverError(QStringLiteral("收到未知消息类型"));
        break;
    }
}

// ==================== 模块：各类型正文处理 ====================
// 功能：解析 type=5 认证响应；认证成功后更新状态并通知界面。
// 失败：正文不是成功 JSON 时断开连接并通知认证失败。
void ChatClient::handleAuthBody(const QByteArray &body)
{
    if (m_state != AuthState::waitingAuthResult)
    {
        return;
    }

    m_auth_timer.stop();

    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject() ||
        !document.object().value("ok").toBool())
    {
        failAuthentication(QStringLiteral("认证响应格式错误"));
        return;
    }
    m_state = AuthState::authenticated;
    if (!sendInitialHistoryQuery())
    {
        return;
    }
    emit authSucceeded();
}

// 功能：解析 type=1 聊天正文，并将完整的消息字段转为界面可用的信号。
// 失败：JSON 或必要字段不合法时通知普通服务端错误。
void ChatClient::handleChatBody(const QByteArray &body)
{
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    {
        emit serverError(QStringLiteral("聊天信息 JSON 格式错误"));
        return;
    }

    const QJsonObject object = document.object();

    const QJsonValue message_id_value = object.value("message_id");
    const QJsonValue local_id_value = object.value("local_id");
    const QJsonValue from_value = object.value("from");
    const QJsonValue to_value = object.value("to");
    const QJsonValue content_value = object.value("content");
    const QJsonValue send_at_value = object.value("send_at");
    if (!message_id_value.isString() || message_id_value.toString().isEmpty() || !local_id_value.isString() ||
        local_id_value.toString().isEmpty() || !from_value.isString() || from_value.toString().isEmpty() ||
        !to_value.isString() || to_value.toString().isEmpty() || !content_value.isString() ||
        content_value.toString().isEmpty() || !send_at_value.isString())
    {
        emit serverError(QStringLiteral("聊天信息缺少必要字段"));
        return;
    }

    const QDateTime send_at = QDateTime::fromString(send_at_value.toString(), Qt::ISODate);
    if (!send_at.isValid())
    {
        emit serverError(QStringLiteral("聊天信息时间字段错误"));
        return;
    }

    const QJsonValue server_received_at_ms_value = object.value(QStringLiteral("server_received_at_ms"));
    const qint64 server_received_at_ms = server_received_at_ms_value.toInteger(-1);
    if (!server_received_at_ms_value.isDouble() || server_received_at_ms < 0 ||
        server_received_at_ms_value.toDouble() != static_cast<double>(server_received_at_ms))
    {
        emit serverError(QStringLiteral("chat 缺少有效的 server_received_at_ms"));
        return;
    }
    const auto message = makeReceivedChatMessage(object);
    emit chatMessageReceived(message);
}
// 功能：解析 type=4 错误正文，并按 local_id 或认证状态转换为对应错误信号。
void ChatClient::handleErrorBody(const QByteArray &body)
{
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parse_error);
    const bool is_valid_error = parse_error.error == QJsonParseError::NoError && document.isObject();

    if (m_state == AuthState::waitingAuthResult)
    {
        const QString reason = is_valid_error
                                   ? document.object().value("message").toString(QStringLiteral("服务器返回错误"))
                                   : QStringLiteral("认证错误响应格式错误");

        failAuthentication(reason);
        return;
    }

    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    {
        emit serverError(QString::fromUtf8(body));
        return;
    }

    const QJsonObject object = document.object();
    const QJsonValue scope_value = object.value(QStringLiteral("scope"));
    const QString local_id = object.value("local_id").toString();
    const QString reason = object.value("message").toString(QStringLiteral("服务器返回错误"));

    if (scope_value.isString() && scope_value.toString() == QStringLiteral("history"))
    {
        clearPendingHistoryRequest();
        emit serverError(reason);
        return;
    }

    if (!local_id.isEmpty())
    {
        emit chatSendFailed(makeMessageStateUpdate(local_id, ChatMessageStatus::Failed, reason));
        return;
    }

    emit serverError(reason);
}

// 功能：收到 type=2 心跳请求后原样发送 type=3 心跳响应。
void ChatClient::handlePingBody(const QByteArray &body)
{
    QString error;
    if (!writeFrame(static_cast<quint8>(protocol::MessageType::pong), body, error))
    {
        m_socket.abort();
    }
}

// 功能：接收 type=3 心跳响应；当前版本不需要保存额外状态。
void ChatClient::handlePongBody(const QByteArray &body)
{
    Q_UNUSED(body);
}

// 功能：解析 type=6 聊天确认正文，并通知界面 local_id 对应消息已被服务器接受。
// 失败：JSON 或确认字段不合法时通知普通服务端错误。
void ChatClient::handleChatAckBody(const QByteArray &body)
{
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    {
        emit serverError(QStringLiteral("聊天确认消息格式错误"));
        return;
    }

    const QJsonObject object = document.object();
    const QJsonValue message_id_value = object.value("message_id");
    const QJsonValue local_id_value = object.value("local_id");
    const QJsonValue status_value = object.value("status");
    if (!message_id_value.isString() || !local_id_value.isString() || !status_value.isString())
    {
        emit serverError(QStringLiteral("聊天确认消息字段错误"));
        return;
    }
    const QString message_id = message_id_value.toString();
    const QString local_id = local_id_value.toString();
    const QString status = status_value.toString();
    if (message_id.isEmpty() || local_id.isEmpty() || status != QStringLiteral("accepted"))
    {
        emit serverError(QStringLiteral("聊天确认消息字段错误"));
        return;
    }
    const QJsonValue server_received_at_value = object.value(QStringLiteral("server_received_at_ms"));
    const qint64 server_received_at_ms = server_received_at_value.toInteger(-1);

    if (!server_received_at_value.isDouble() || server_received_at_ms < 0 ||
        server_received_at_value.toDouble() != static_cast<double>(server_received_at_ms))
    {
        emit serverError(QStringLiteral("chat 缺少有效的 server_received_at_ms"));
        return;
    }
    emit chatMessageAccepted(
        makeMessageStateUpdate(local_id, ChatMessageStatus::Accepted, {}, message_id, server_received_at_ms));
}

// 功能：校验服务端返回的最终送达状态，并通知界面更新对应已有消息。
void ChatClient::handleDeliveryReceiptBody(const QByteArray &body)
{
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    {
        emit serverError(QString::fromUtf8(body));
        return;
    }
    const QJsonObject object = document.object();

    const QJsonValue local_id_value = object.value("local_id");
    const QJsonValue status_value = object.value("status");
    // toString() 会把非字符串悄悄转换为空串,先显式类型校验
    if (!local_id_value.isString() || !status_value.isString())
    {
        emit serverError(QStringLiteral("送达确认消息字段类型错误"));
        return;
    }

    const QString local_id = local_id_value.toString().trimmed();
    const QString status = status_value.toString();
    if (local_id.isEmpty() || status != QStringLiteral("delivered"))
    {
        emit serverError(QStringLiteral("投递确认消息字段错误"));
        return;
    }
    emit chatMessageDelivered(makeMessageStateUpdate(local_id, ChatMessageStatus::Delivered));
}

// 功能：校验并缓存服务端在线用户快照，成功后通知界面整体刷新。
void ChatClient::handleOnlineUsersBody(const QByteArray &body)
{
    const auto reject_snapshot = [this](const QString &reason) {
        clearOnlineUsers();
        emit serverError(reason);
    };

    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(body, &parse_error);

    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    {
        reject_snapshot(QStringLiteral("在线用户快照 JSON 格式错误"));
        return;
    }

    const QJsonObject object = document.object();
    const QJsonValue users_value = object.value(QStringLiteral("users"));
    if (!users_value.isArray())
    {
        reject_snapshot(QStringLiteral("在线用户列表字段错误"));
        return;
    }
    QStringList users;
    // 存储不重复的元素
    QSet<QString> seen_users;
    const QJsonArray users_array = users_value.toArray();

    if (users_array.size() > static_cast<qsizetype>(protocol::kMaxOnlineUsersSnapshotCount))
    {
        reject_snapshot(QStringLiteral("在线用户列表超过人数上限"));
        return;
    }

    for (const QJsonValue &user_value : users_array)
    {
        const QString username = user_value.toString();
        const std::string username_utf8 = username.toUtf8().toStdString();

        if (!user_value.isString() || !protocol::isValidUsername(username_utf8))
        {
            reject_snapshot(QStringLiteral("在线用户列表包含非法用户名"));
            return;
        }
        if (seen_users.contains(username))
        {
            reject_snapshot(QStringLiteral("在线用户列表包含重复用户名"));
            return;
        }
        seen_users.insert(username);
        users.append(username);
    }

    m_online_users = users;
    emit onlineUsersChanged(users);
}

void ChatClient::handleHistoryResultBody(const QByteArray &body)
{

    // 发生“当前查询内部错误”时，丢弃已收到的半页，避免向 UI 暴露不完整历史。
    const auto fail_current_history = [this](const QString &reason) {
        clearPendingHistoryRequest();
        emit serverError(reason);
    };

    // JSON 必须是对象
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    {
        fail_current_history(QStringLiteral("历史响应 JSON 格式错误"));
        return;
    }
    const auto object = document.object();

    // 2. 先验证并关联 request_id；无法关联的响应只报告，不能取消仍在等待的合法请求。
    const auto request_id_value = object.value("request_id");
    if (!request_id_value.isString() || request_id_value.toString().isEmpty())
    {
        emit serverError(QStringLiteral("历史响应缺少 request_id"));
        return;
    }
    const QString request_id = request_id_value.toString();
    if (m_active_history_request_id.isEmpty() || request_id != m_active_history_request_id)
    {
        emit serverError(QStringLiteral("没有对应请求的历史相应或历史响应 request_id 与当前请求不匹配"));
        return;
    }

    // 3. 校验每个分块都必须具有的字段
    const auto messages_value = object.value("messages");
    const auto is_last_chunk_value = object.value("is_last_chunk");
    if (!messages_value.isArray() || !is_last_chunk_value.isBool())
    {
        fail_current_history(QStringLiteral("历史响应分块字段错误"));
        return;
    }
    const QJsonArray messages_array = messages_value.toArray();
    const bool is_last_chunk = is_last_chunk_value.toBool();

    if (!is_last_chunk && messages_array.isEmpty())
    {
        fail_current_history(QStringLiteral("非最终历史分块不能为空"));
        return;
    }

    // 4. 先把本块的每条消息全部验证并转换成功，再追加到总暂存区。
    QList<ChatMessage> chunk_messages;
    bool has_more = false;
    for (const QJsonValue &message_value : messages_array)
    {
        if (!message_value.isObject())
        {
            fail_current_history(QStringLiteral("历史消息必须是对象"));
            return;
        }
        const auto message_object = message_value.toObject();
        const auto message_id_value = message_object.value(QStringLiteral("message_id"));
        const auto local_id_value = message_object.value(QStringLiteral("local_id"));
        const auto from_value = message_object.value(QStringLiteral("from"));
        const auto to_value = message_object.value(QStringLiteral("to"));
        const auto content_value = message_object.value(QStringLiteral("content"));
        const auto send_at_value = message_object.value(QStringLiteral("send_at"));
        const auto server_received_at_value = message_object.value(QStringLiteral("server_received_at_ms"));

        if (!message_id_value.isString() || message_id_value.toString().isEmpty() || !local_id_value.isString() ||
            local_id_value.toString().isEmpty() || !from_value.isString() || from_value.toString().isEmpty() ||
            !to_value.isString() || to_value.toString().isEmpty() || !content_value.isString() ||
            content_value.toString().isEmpty() || !send_at_value.isString() || send_at_value.toString().isEmpty() ||
            !server_received_at_value.isDouble())
        {
            fail_current_history(QStringLiteral("历史消息缺少必要字段"));
            return;
        }
        const QDateTime send_at = QDateTime::fromString(send_at_value.toString(), Qt::ISODate);
        if (!send_at.isValid())
        {
            fail_current_history(QStringLiteral("历史消息时间字段错误"));
            return;
        }

        const qint64 server_received_at = server_received_at_value.toInteger(-1);
        if (server_received_at < 0 || server_received_at_value.toDouble() != static_cast<double>(server_received_at))
        {
            fail_current_history(QStringLiteral("历史消息服务端时间字段错误"));
            return;
        }
        chunk_messages.append(makeReceivedChatMessage(message_object));
    }

    // 5. 非最终块不能携带翻页结论。
    if (!is_last_chunk)
    {
        if (object.contains(QStringLiteral("has_more")) || object.contains(QStringLiteral("next_cursor")))
        {
            fail_current_history(QStringLiteral("非最终历史分块不应包含翻页结果"));
            return;
        }
    } // 6. 最终块必须完整给出 has_more 和 next_cursor。
    else
    {
        const auto has_more_value = object.value(QStringLiteral("has_more"));
        const auto next_cursor_value = object.value(QStringLiteral("next_cursor"));

        if (!object.contains(QStringLiteral("has_more")) || !has_more_value.isBool() ||
            !object.contains(QStringLiteral("next_cursor")))
        {
            fail_current_history(QStringLiteral("最终历史分块缺少翻页结果"));
            return;
        }
        has_more = has_more_value.toBool();
        // 只有确认 has_more == true，才能读取游标对象内部字段
        if (!has_more)
        {
            if (!next_cursor_value.isNull())
            {
                fail_current_history(QStringLiteral("无下一页时游标必须为 null"));
                return;
            }
        }
        else
        {
            if (!next_cursor_value.isObject())
            {
                fail_current_history(QStringLiteral("下一页游标字段错误"));
                return;
            }
            const auto cursor_object = next_cursor_value.toObject();
            const auto timestamp_value = cursor_object.value(QStringLiteral("server_received_at_ms"));
            const auto cursor_message_id_value = cursor_object.value(QStringLiteral("message_id"));

            const auto timestamp = timestamp_value.toInteger(-1);
            if (!timestamp_value.isDouble() || timestamp < 0 ||
                timestamp_value.toDouble() != static_cast<double>(timestamp) || !cursor_message_id_value.isString() ||
                cursor_message_id_value.toString().isEmpty())
            {
                fail_current_history(QStringLiteral("下一页游标内容错误"));
                return;
            }
        }
    }
    if (m_pending_history_messages.size() + chunk_messages.size() > kInitialHistoryLimit)
    {
        fail_current_history(QStringLiteral("历史响应消息数量超过首屏上限"));
        return;
    }
    // 7. 到这里才允许提交本块；最终块再一次性发给上层。
    m_pending_history_messages.append(chunk_messages);
    if (!is_last_chunk)
    {
        return;
    }
    const auto completed_messages = m_pending_history_messages;
    clearPendingHistoryRequest();
    emit historyPageReceived(completed_messages, has_more);
}

// 功能：清空已失效的在线快照；仅在状态变化时发送空快照通知。
void ChatClient::clearOnlineUsers()
{
    if (m_online_users.isEmpty())
    {
        return;
    }

    m_online_users.clear();
    emit onlineUsersChanged({});
}

// 功能：清空当前的待处理历史请求。
void ChatClient::clearPendingHistoryRequest()
{
    m_pending_history_messages.clear();
    m_active_history_request_id.clear();
}
