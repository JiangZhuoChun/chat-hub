#include "../chatclient.h"
#include "../mainwindow.h"

#include <QApplication>
#include <QLabel>
#include <QLayout>
#include <QListWidget>

#include <iostream>

namespace
{

// 功能：构造一条已由服务端持久化的对方消息，供会话排序回归测试使用。
ChatMessage makeReceivedMessage(const QString &message_id, const QString &local_id, const QString &content,
                                const qint64 server_received_at_ms)
{
    ChatMessage message;
    message.message_id = message_id;
    message.local_id = local_id;
    message.from = QStringLiteral("bob");
    message.to = QStringLiteral("alice");
    message.content = content;
    message.send_at = QDateTime::fromString(QStringLiteral("2026-08-17T10:00:00.000Z"), Qt::ISODate);
    message.server_received_at_ms = server_received_at_ms;
    message.status = ChatMessageStatus::Received;
    return message;
}

// 功能：点击唯一会话并按气泡布局顺序读取 local_id，用于验证实际渲染顺序。
QStringList displayedLocalIds(MainWindow &window)
{
    auto *conversation_list = window.findChild<QListWidget *>(QStringLiteral("conversationList"));
    if (conversation_list == nullptr || conversation_list->count() != 1)
    {
        return {};
    }
    conversation_list->itemClicked(conversation_list->item(0));

    auto *message_container = window.findChild<QWidget *>(QStringLiteral("messageContainer"));
    if (message_container == nullptr || message_container->layout() == nullptr)
    {
        return {};
    }

    QStringList local_ids;
    QLayout *layout = message_container->layout();
    for (int index = 1; index < layout->count(); ++index)
    {
        QWidget *row = layout->itemAt(index)->widget();
        auto *label = row == nullptr ? nullptr : row->findChild<QLabel *>();
        if (label == nullptr)
        {
            return {};
        }
        local_ids.append(label->property("local_id").toString());
    }
    return local_ids;
}

// 功能：验证实时 M3 先到后，历史 M1/M2/M3 合并仍按服务端时间正序显示。
bool testHistoryAfterRealtimeKeepsAscendingOrder()
{
    ChatClient client;
    MainWindow window(&client, QStringLiteral("alice"));

    const ChatMessage message_3 =
        makeReceivedMessage(QStringLiteral("message-3"), QStringLiteral("local-3"), QStringLiteral("M3"), 300);
    client.chatMessageReceived(message_3);

    const QList<ChatMessage> history_messages{
        makeReceivedMessage(QStringLiteral("message-1"), QStringLiteral("local-1"), QStringLiteral("M1"), 100),
        makeReceivedMessage(QStringLiteral("message-2"), QStringLiteral("local-2"), QStringLiteral("M2"), 200),
        message_3};
    client.historyPageReceived(history_messages, false);

    const QStringList local_ids = displayedLocalIds(window);
    if (local_ids != QStringList{QStringLiteral("local-1"), QStringLiteral("local-2"), QStringLiteral("local-3")})
    {
        std::cerr << "历史与实时消息没有按服务端时间正序显示\n";
        return false;
    }
    return true;
}

// 功能：验证本地待发送消息收到确认时间后，会从末尾重新定位到正确位置。
bool testAcceptedMessageReordersLocalPendingMessage()
{
    ChatClient client;
    MainWindow window(&client, QStringLiteral("alice"));

    client.chatMessageReceived(
        makeReceivedMessage(QStringLiteral("message-2"), QStringLiteral("local-2"), QStringLiteral("M2"), 200));

    ChatMessage pending_message;
    pending_message.local_id = QStringLiteral("local-1");
    pending_message.from = QStringLiteral("alice");
    pending_message.to = QStringLiteral("bob");
    pending_message.content = QStringLiteral("M1");
    pending_message.send_at = QDateTime::currentDateTimeUtc();
    pending_message.status = ChatMessageStatus::Sending;
    client.chatMessageQueued(pending_message);

    ChatMessage accepted_update;
    accepted_update.message_id = QStringLiteral("message-1");
    accepted_update.local_id = QStringLiteral("local-1");
    accepted_update.status = ChatMessageStatus::Accepted;
    accepted_update.server_received_at_ms = 100;
    client.chatMessageAccepted(accepted_update);

    const QStringList local_ids = displayedLocalIds(window);
    if (local_ids != QStringList{QStringLiteral("local-1"), QStringLiteral("local-2")})
    {
        std::cerr << "已确认的本地消息没有按服务端时间重新定位\n";
        return false;
    }
    return true;
}

// 功能：验证服务端时间相同时，以 message_id 作为稳定的第二排序键。
bool testSameTimestampUsesMessageIdAsTieBreaker()
{
    ChatClient client;
    MainWindow window(&client, QStringLiteral("alice"));

    client.chatMessageReceived(
        makeReceivedMessage(QStringLiteral("message-b"), QStringLiteral("local-b"), QStringLiteral("B"), 300));
    client.chatMessageReceived(
        makeReceivedMessage(QStringLiteral("message-a"), QStringLiteral("local-a"), QStringLiteral("A"), 300));

    const QStringList local_ids = displayedLocalIds(window);
    if (local_ids != QStringList{QStringLiteral("local-a"), QStringLiteral("local-b")})
    {
        std::cerr << "相同服务端时间没有按 message_id 稳定排序\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);

    if (testHistoryAfterRealtimeKeepsAscendingOrder() && testAcceptedMessageReordersLocalPendingMessage() &&
        testSameTimestampUsesMessageIdAsTieBreaker())
    {
        std::cout << "PASS: conversation ordering after history merge and acceptance\n";
        return 0;
    }

    std::cerr << "FAIL: conversation ordering after history merge and acceptance\n";
    return 1;
}
