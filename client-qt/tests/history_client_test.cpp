#include "../chatclient.h"

#include "protocol/chat_protocol.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <functional>
#include <iostream>

namespace
{

struct ReceivedFrame
{
    quint8 type = 0;
    QByteArray body;
};

// 功能：将原始正文编码为 ChatHub 协议帧，供测试中的本地 TCP 对端发送。
QByteArray makeFrame(const quint8 type, const QByteArray &body)
{
    const auto length = static_cast<quint32>(body.size());

    QByteArray frame;
    frame.reserve(static_cast<int>(protocol::kFrameHeaderLength) + body.size());
    frame.append(static_cast<char>(protocol::kFrameMagic >> 8));
    frame.append(static_cast<char>(protocol::kFrameMagic & 0xFF));
    frame.append(static_cast<char>(protocol::kProtocolVersion));
    frame.append(static_cast<char>(type));
    frame.append(static_cast<char>((length >> 24) & 0xFF));
    frame.append(static_cast<char>((length >> 16) & 0xFF));
    frame.append(static_cast<char>((length >> 8) & 0xFF));
    frame.append(static_cast<char>(length & 0xFF));
    frame.append(body);
    return frame;
}

// 功能：将 JSON 对象序列化后编码为 ChatHub 协议帧。
QByteArray makeFrame(const quint8 type, const QJsonObject &object)
{
    return makeFrame(type, QJsonDocument(object).toJson(QJsonDocument::Compact));
}

// 功能：从测试对端的接收缓存取出一个完整帧，保留半包直到数据完整。
bool tryTakeFrame(QByteArray &buffer, ReceivedFrame &frame)
{
    if (buffer.size() < static_cast<int>(protocol::kFrameHeaderLength))
    {
        return false;
    }

    const auto *header = reinterpret_cast<const unsigned char *>(buffer.constData());
    const auto magic = static_cast<quint16>((header[0] << 8) | header[1]);
    const auto body_length = (static_cast<quint32>(header[4]) << 24) | (static_cast<quint32>(header[5]) << 16) |
                             (static_cast<quint32>(header[6]) << 8) | static_cast<quint32>(header[7]);
    if (magic != protocol::kFrameMagic || header[2] != protocol::kProtocolVersion ||
        body_length > protocol::kMaxFrameBodyLength)
    {
        return false;
    }

    const int frame_length = static_cast<int>(protocol::kFrameHeaderLength + body_length);
    if (buffer.size() < frame_length)
    {
        return false;
    }

    frame.type = header[3];
    frame.body = buffer.mid(static_cast<int>(protocol::kFrameHeaderLength), static_cast<int>(body_length));
    buffer.remove(0, frame_length);
    return true;
}

// 功能：在不阻塞 Qt 事件循环的前提下，等待异步网络事件到达。
bool waitUntil(const std::function<bool()> &condition, QDeadlineTimer deadline = QDeadlineTimer(1000))
{
    while (!deadline.hasExpired())
    {
        if (condition())
        {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return condition();
}

// 功能：构造满足 history_result 合同的一条历史消息 JSON 对象。
QJsonObject makeHistoryMessage(const QString &message_id, const QString &local_id)
{
    QJsonObject message;
    message.insert(QStringLiteral("message_id"), message_id);
    message.insert(QStringLiteral("local_id"), local_id);
    message.insert(QStringLiteral("from"), QStringLiteral("alice"));
    message.insert(QStringLiteral("to"), QStringLiteral("bob"));
    message.insert(QStringLiteral("content"), message_id + QStringLiteral(" content"));
    message.insert(QStringLiteral("send_at"), QStringLiteral("2026-08-17T10:00:00.000Z"));
    message.insert(QStringLiteral("server_received_at_ms"), 1786951200000.0);
    return message;
}

// 功能：服务端保持连接但不返回认证结果时，客户端应在认证等待超时后只报告一次认证失败。
bool testAuthenticationResponseTimeout()
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 9000))
    {
        std::cerr << "无法监听客户端固定测试端口 9000：" << server.errorString().toStdString() << '\n';
        return false;
    }

    QTcpSocket *peer = nullptr;
    QByteArray peer_buffer;
    QObject peer_owner;
    int auth_frame_count = 0;
    QElapsedTimer auth_frame_timer;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        peer = server.nextPendingConnection();
        peer->setParent(&peer_owner);
        QObject::connect(peer, &QTcpSocket::readyRead, peer, [&] {
            peer_buffer.append(peer->readAll());

            ReceivedFrame frame;
            while (tryTakeFrame(peer_buffer, frame))
            {
                if (frame.type == static_cast<quint8>(protocol::MessageType::auth))
                {
                    ++auth_frame_count;
                    auth_frame_timer.start();
                }
            }
        });
    });

    ChatClient client;
    int auth_failed_count = 0;
    QString failure_reason;
    QObject::connect(&client, &ChatClient::authFailed, &client, [&](const QString &reason) {
        ++auth_failed_count;
        failure_reason = reason;
    });

    client.connectWithToken(QStringLiteral("test-token"));
    if (!waitUntil([&] { return peer != nullptr; }))
    {
        std::cerr << "客户端没有建立 TCP 连接\n";
        return false;
    }

    if (!waitUntil([&] { return auth_frame_count == 1; }) || !auth_frame_timer.isValid())
    {
        std::cerr << "客户端没有先发送认证帧\n";
        return false;
    }

    if (!waitUntil([&] { return auth_failed_count == 1; }, QDeadlineTimer(6500)) ||
        failure_reason != QStringLiteral("等待 chat-server 认证响应超时") || client.isAuthenticated() ||
        auth_frame_timer.elapsed() < 4500)
    {
        std::cerr << "客户端没有按认证响应超时规则失败\n";
        return false;
    }

    if (!waitUntil([&] { return peer->state() == QAbstractSocket::UnconnectedState; }))
    {
        std::cerr << "认证超时后测试连接没有关闭\n";
        return false;
    }

    const QDeadlineTimer duplicate_signal_deadline(150);
    while (!duplicate_signal_deadline.hasExpired())
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    if (auth_failed_count != 1)
    {
        std::cerr << "认证超时重复通知认证失败\n";
        return false;
    }
    return true;
}

// 功能：验证实时 chat 和 chat_ack 都将服务端排序时间传递到客户端消息模型。
bool testRealtimeMessageAndAckKeepServerReceivedTime()
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 9000))
    {
        std::cerr << "无法监听客户端固定测试端口 9000：" << server.errorString().toStdString() << '\n';
        return false;
    }

    QTcpSocket *peer = nullptr;
    QByteArray peer_buffer;
    QObject peer_owner;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        peer = server.nextPendingConnection();
        peer->setParent(&peer_owner);
        QObject::connect(peer, &QTcpSocket::readyRead, peer, [&] { peer_buffer.append(peer->readAll()); });
    });

    ChatClient client;
    int server_error_count = 0;
    int received_count = 0;
    int accepted_count = 0;
    int auth_succeeded_count = 0;
    ChatMessage received_message;
    ChatMessage accepted_update;
    QObject::connect(&client, &ChatClient::serverError, &client, [&](const QString &) { ++server_error_count; });
    QObject::connect(&client, &ChatClient::chatMessageReceived, &client, [&](const ChatMessage &message) {
        ++received_count;
        received_message = message;
    });
    QObject::connect(&client, &ChatClient::chatMessageAccepted, &client, [&](const ChatMessage &update) {
        ++accepted_count;
        accepted_update = update;
    });
    QObject::connect(&client, &ChatClient::authSucceeded, &client, [&] { ++auth_succeeded_count; });

    client.connectWithToken(QStringLiteral("test-token"));
    if (!waitUntil([&] { return peer != nullptr; }))
    {
        std::cerr << "客户端没有建立 TCP 连接\n";
        return false;
    }

    ReceivedFrame auth_frame;
    if (!waitUntil([&] { return tryTakeFrame(peer_buffer, auth_frame); }) ||
        auth_frame.type != static_cast<quint8>(protocol::MessageType::auth))
    {
        std::cerr << "没有收到认证帧\n";
        return false;
    }

    QJsonObject auth_result;
    auth_result.insert(QStringLiteral("ok"), true);
    peer->write(makeFrame(static_cast<quint8>(protocol::MessageType::auth), auth_result));
    peer->flush();

    if (!waitUntil([&] { return auth_succeeded_count == 1; }))
    {
        std::cerr << "客户端没有完成认证\n";
        return false;
    }

    constexpr qint64 kServerReceivedAtMs = 1786951200123;
    QJsonObject chat = makeHistoryMessage(QStringLiteral("message-realtime"), QStringLiteral("local-realtime"));
    chat.insert(QStringLiteral("server_received_at_ms"), static_cast<double>(kServerReceivedAtMs));
    peer->write(makeFrame(static_cast<quint8>(protocol::MessageType::chat), chat));
    peer->flush();

    if (!waitUntil([&] { return received_count == 1; }) || !received_message.server_received_at_ms.has_value() ||
        received_message.server_received_at_ms.value() != kServerReceivedAtMs)
    {
        std::cerr << "实时 chat 没有保留 server_received_at_ms\n";
        return false;
    }

    QJsonObject ack;
    ack.insert(QStringLiteral("message_id"), QStringLiteral("message-ack"));
    ack.insert(QStringLiteral("local_id"), QStringLiteral("local-ack"));
    ack.insert(QStringLiteral("status"), QStringLiteral("accepted"));
    ack.insert(QStringLiteral("server_received_at_ms"), static_cast<double>(kServerReceivedAtMs));
    peer->write(makeFrame(static_cast<quint8>(protocol::MessageType::chat_ack), ack));
    peer->flush();

    if (!waitUntil([&] { return accepted_count == 1; }) || !accepted_update.server_received_at_ms.has_value() ||
        accepted_update.server_received_at_ms.value() != kServerReceivedAtMs)
    {
        std::cerr << "chat_ack 没有保留 server_received_at_ms\n";
        return false;
    }

    QJsonObject invalid_chat = chat;
    invalid_chat.insert(QStringLiteral("server_received_at_ms"), 1.5);
    peer->write(makeFrame(static_cast<quint8>(protocol::MessageType::chat), invalid_chat));

    QJsonObject invalid_ack = ack;
    invalid_ack.remove(QStringLiteral("server_received_at_ms"));
    peer->write(makeFrame(static_cast<quint8>(protocol::MessageType::chat_ack), invalid_ack));

    QJsonObject negative_chat = chat;
    negative_chat.insert(QStringLiteral("server_received_at_ms"), -1.0);
    peer->write(makeFrame(static_cast<quint8>(protocol::MessageType::chat), negative_chat));
    peer->flush();

    if (!waitUntil([&] { return server_error_count == 3; }) || received_count != 1 || accepted_count != 1)
    {
        std::cerr << "非法 server_received_at_ms 没有被拒绝\n";
        return false;
    }

    client.disconnectFromServer();
    if (!waitUntil([&] { return peer->state() == QAbstractSocket::UnconnectedState; }))
    {
        std::cerr << "客户端测试连接没有正常关闭\n";
        return false;
    }
    return true;
}

// 功能：验证 C1 主链路：认证成功后请求历史，收齐多块结果后只通知一次完整页面。
bool testInitialHistoryQueryAndChunkAggregation()
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 9000))
    {
        std::cerr << "无法监听客户端固定测试端口 9000：" << server.errorString().toStdString() << '\n';
        return false;
    }

    QTcpSocket *peer = nullptr;
    QByteArray peer_buffer;
    QObject peer_owner;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        peer = server.nextPendingConnection();
        peer->setParent(&peer_owner);
        QObject::connect(peer, &QTcpSocket::readyRead, peer, [&] { peer_buffer.append(peer->readAll()); });
    });

    ChatClient client;
    int history_signal_count = 0;
    int server_error_count = 0;
    QList<ChatMessage> completed_messages;
    bool completed_has_more = true;
    QObject::connect(&client, &ChatClient::historyPageReceived, &client,
                     [&](const QList<ChatMessage> &messages, const bool has_more) {
                         ++history_signal_count;
                         completed_messages = messages;
                         completed_has_more = has_more;
                     });
    QObject::connect(&client, &ChatClient::serverError, &client, [&](const QString &) { ++server_error_count; });

    client.connectWithToken(QStringLiteral("test-token"));
    if (!waitUntil([&] { return peer != nullptr; }))
    {
        std::cerr << "客户端没有建立 TCP 连接\n";
        return false;
    }

    ReceivedFrame auth_frame;
    if (!waitUntil([&] { return tryTakeFrame(peer_buffer, auth_frame); }) ||
        auth_frame.type != static_cast<quint8>(protocol::MessageType::auth))
    {
        std::cerr << "没有收到认证帧\n";
        return false;
    }

    QJsonObject auth_result;
    auth_result.insert(QStringLiteral("ok"), true);
    peer->write(makeFrame(static_cast<quint8>(protocol::MessageType::auth), auth_result));
    peer->flush();

    ReceivedFrame history_query_frame;
    if (!waitUntil([&] { return tryTakeFrame(peer_buffer, history_query_frame); }) ||
        history_query_frame.type != static_cast<quint8>(protocol::MessageType::history_query))
    {
        std::cerr << "认证成功后没有收到 history_query\n";
        return false;
    }

    QJsonParseError parse_error;
    const QJsonDocument query_document = QJsonDocument::fromJson(history_query_frame.body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !query_document.isObject())
    {
        std::cerr << "history_query 不是合法 JSON 对象\n";
        return false;
    }
    const QJsonObject query = query_document.object();
    const QJsonValue request_id_value = query.value(QStringLiteral("request_id"));
    const QJsonValue limit_value = query.value(QStringLiteral("limit"));
    if (!request_id_value.isString() || request_id_value.toString().isEmpty() || !limit_value.isDouble() ||
        limit_value.toInteger(-1) != 50)
    {
        std::cerr << "history_query 的 request_id 或 limit 不符合首屏合同\n";
        return false;
    }

    // 无 request_id 的响应不能证明属于当前请求；客户端应报告错误但保留有效请求状态。
    QJsonObject missing_request_id_chunk;
    missing_request_id_chunk.insert(QStringLiteral("messages"), QJsonArray{});
    missing_request_id_chunk.insert(QStringLiteral("is_last_chunk"), true);
    missing_request_id_chunk.insert(QStringLiteral("has_more"), false);
    missing_request_id_chunk.insert(QStringLiteral("next_cursor"), QJsonValue(QJsonValue::Null));
    peer->write(makeFrame(static_cast<quint8>(protocol::MessageType::history_result), missing_request_id_chunk));
    peer->write(makeFrame(static_cast<quint8>(protocol::MessageType::ping), QByteArray{}));
    peer->flush();

    ReceivedFrame malformed_chunk_pong;
    if (!waitUntil([&] { return tryTakeFrame(peer_buffer, malformed_chunk_pong); }) ||
        malformed_chunk_pong.type != static_cast<quint8>(protocol::MessageType::pong) || server_error_count != 1)
    {
        std::cerr << "无 request_id 的响应没有被报告为非当前请求错误\n";
        return false;
    }

    QJsonObject first_chunk;
    first_chunk.insert(QStringLiteral("request_id"), request_id_value.toString());
    QJsonArray first_chunk_messages;
    first_chunk_messages.append(makeHistoryMessage(QStringLiteral("message-1"), QStringLiteral("local-1")));
    first_chunk.insert(QStringLiteral("messages"), first_chunk_messages);
    first_chunk.insert(QStringLiteral("is_last_chunk"), false);
    peer->write(makeFrame(static_cast<quint8>(protocol::MessageType::history_result), first_chunk));
    peer->write(makeFrame(static_cast<quint8>(protocol::MessageType::ping), QByteArray{}));
    peer->flush();

    ReceivedFrame pong_frame;
    if (!waitUntil([&] { return tryTakeFrame(peer_buffer, pong_frame); }) ||
        pong_frame.type != static_cast<quint8>(protocol::MessageType::pong))
    {
        std::cerr << "客户端没有在处理首个历史分块后响应 ping\n";
        return false;
    }
    if (history_signal_count != 0)
    {
        std::cerr << "非最终历史分块不应发射页面信号\n";
        return false;
    }
    if (server_error_count != 1)
    {
        std::cerr << "无 request_id 的响应不应取消有效历史请求\n";
        return false;
    }

    QJsonObject final_chunk;
    final_chunk.insert(QStringLiteral("request_id"), request_id_value.toString());
    QJsonArray final_chunk_messages;
    final_chunk_messages.append(makeHistoryMessage(QStringLiteral("message-2"), QStringLiteral("local-2")));
    final_chunk.insert(QStringLiteral("messages"), final_chunk_messages);
    final_chunk.insert(QStringLiteral("is_last_chunk"), true);
    final_chunk.insert(QStringLiteral("has_more"), false);
    final_chunk.insert(QStringLiteral("next_cursor"), QJsonValue(QJsonValue::Null));
    peer->write(makeFrame(static_cast<quint8>(protocol::MessageType::history_result), final_chunk));
    peer->flush();

    if (!waitUntil([&] { return history_signal_count == 1; }))
    {
        std::cerr << "最终历史分块没有发射页面信号\n";
        return false;
    }
    if (completed_has_more || completed_messages.size() != 2 ||
        completed_messages.at(0).message_id != QStringLiteral("message-1") ||
        completed_messages.at(1).message_id != QStringLiteral("message-2") ||
        !completed_messages.at(0).server_received_at_ms.has_value() ||
        !completed_messages.at(1).server_received_at_ms.has_value() ||
        completed_messages.at(0).server_received_at_ms.value() != 1786951200000 ||
        completed_messages.at(1).server_received_at_ms.value() != 1786951200000 ||
        completed_messages.at(0).status != ChatMessageStatus::Received ||
        completed_messages.at(1).status != ChatMessageStatus::Received)
    {
        std::cerr << "历史页面内容、排序时间、顺序或状态错误\n";
        return false;
    }

    if (history_signal_count != 1)
    {
        std::cerr << "历史页面信号被重复发射\n";
        return false;
    }

    client.disconnectFromServer();
    if (!waitUntil([&] { return peer->state() == QAbstractSocket::UnconnectedState; }))
    {
        std::cerr << "客户端测试连接没有正常关闭\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    if (testAuthenticationResponseTimeout() && testRealtimeMessageAndAckKeepServerReceivedTime() &&
        testInitialHistoryQueryAndChunkAggregation())
    {
        std::cout << "PASS: auth timeout, realtime timestamps and initial history query\n";
        return 0;
    }

    std::cerr << "FAIL: realtime timestamps or initial history query\n";
    return 1;
}
