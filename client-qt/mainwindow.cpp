#include "mainwindow.h"

#include "chatclient.h"
#include "ui_mainwindow.h"

#include <QLabel>
#include <QLayoutItem>
#include <QListWidgetItem>
#include <QSet>
#include <QStyle>
#include <QUuid>
#include <algorithm>
#include <utility>
namespace
{
constexpr int kConversationPreviewMaxLength = 24;

// 功能：判断 left 是否应排在 right 前；持久化消息按服务端时间和 message_id 正序排列。
bool messageComesBefore(const ChatMessage &left, const ChatMessage &right)
{
    const bool left_has_time = left.server_received_at_ms.has_value();
    if (const bool right_has_time = right.server_received_at_ms.has_value(); left_has_time != right_has_time)
    {
        return left_has_time; // 有服务端时间的排在前面
    }
    if (!left_has_time)
    {
        return false;
    }

    const qint64 left_time = left.server_received_at_ms.value();
    const qint64 right_time = right.server_received_at_ms.value();
    if (left_time != right_time)
    {
        return left_time < right_time; // 服务端时间早的排在前面
    }
    return left.message_id < right.message_id; // message_id 早的排在前面
}
} // namespace

// ==================== 模块：窗口生命周期 ====================
// 功能：创建主窗口、初始化界面状态，并连接用户操作和聊天客户端信号。
MainWindow::MainWindow(ChatClient *chat_client, QString username, QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), m_chat(chat_client), m_username(std::move(username))
{
    Q_ASSERT(m_chat != nullptr);
    ui->setupUi(this);
    setupUiState();
    connectSlots();
    updateOnlineUsers(m_chat->onlineUsers());
}
// 功能：释放 Qt 设计器创建的主窗口界面对象。
MainWindow::~MainWindow()
{
    delete ui;
}

// 1.连接信号槽
//  ==================== 模块：窗口与连接状态 ====================
//  功能：连接断开时禁用发送按钮，并提示用户重新登录。
void MainWindow::onDisconnected()
{
    updateConnectionState(false, QStringLiteral("连接已断开，请重新登录"));
    updateOnlineUsers(QStringList{});
}

// 2.用户操作槽
//  ==================== 模块：用户发送操作 ====================
//  功能：读取当前输入框中的接收者和正文，并请求 ChatClient 发送消息。
void MainWindow::onSendClicked()
{
    // 手填接收者优先；未填写时才使用当前正在查看的会话联系人。
    const QString to = ui->recipientEdit->text().trimmed();
    const QString content = ui->messageEdit->toPlainText();
    QString real_to = to;

    if (to.isEmpty())
    {
        if (m_currentPeer.isEmpty())
        {
            statusBar()->showMessage("请选择联系人或输入接收者");
            return;
        }
        real_to = m_currentPeer;
    }
    // 与 ChatClient 保持一致：空字符串、空格和换行都不允许进入发送链路。
    if (content.trimmed().isEmpty())
    {
        // 仅用 trimmed() 判断，不修改真正发出的正文，保留用户输入的有效首尾空格。
        statusBar()->showMessage("信息不能为空");
        return;
    }
    // 所有校验通过先保存再发送
    const ChatMessage message = makeOutgoingChatMessage(real_to, content);
    m_conversations[message.to].append(message);
    // 1.确保会话列表项存在
    ensureConversationItem(message.to);
    // 2. 移动会话列表项到顶部
    moveConversationItemToTop(message.to);
    // 3. 刷新会话列表项
    refreshConversationItem(message.to);

    if (m_currentPeer == message.to)
    {
        renderCurrentConversation();
    }
    m_chat->sendChatMessage(message);
}
// 功能：读取待确认表中的原消息，用相同 local_id 请求 ChatClient 再次发送。
void MainWindow::onRetryClicked(const QString &local_id)
{
    ChatMessage *message = findMessageByLocalId(local_id);
    if (message == nullptr)
    {
        statusBar()->showMessage(QStringLiteral("未找到需要重试的消息"));
        return;
    }
    // 网络调用可能同步触发信号，因此先复制，后续不再使用 message 指针。
    const auto to = message->to;
    message->status = ChatMessageStatus::Sending;
    message->failure_reason.clear();
    const ChatMessage retry_message = *message;

    if (m_currentPeer == to)
    {
        renderCurrentConversation();
    }
    m_chat->sendChatMessage(retry_message);
}
// 功能：点击会话列表项时，更新当前会话并渲染聊天界面。
void MainWindow::onConversationItemClicked(const QListWidgetItem *item)
{
    if (item == nullptr)
    {
        return;
    }

    m_currentPeer = item->data(Qt::UserRole).toString();
    markConversationRead(m_currentPeer);
    ui->peerNameLabel->setText(m_currentPeer);
    renderCurrentConversation();
}

// 功能：读取在线列表项保存的用户名，仅回填接收者输入框。
void MainWindow::onOnlineUserItemClicked(const QListWidgetItem *item)
{
    if (item == nullptr)
    {
        return;
    }

    const auto username = item->data(Qt::UserRole).toString();
    if (username.isEmpty())
    {
        return;
    }

    ui->recipientEdit->setText(username);
    ui->statusbar->showMessage(QStringLiteral("已选择联系人：") + username);
}

// 3.消息状态槽
//  ==================== 模块：消息发送状态处理 ====================
//  功能：为已进入发送缓冲区的消息创建气泡，或在重试时恢复已有气泡的发送状态。
void MainWindow::onChatMessageQueued(const ChatMessage &message)
{
    if (ChatMessage *chat_message = findMessageByLocalId(message.local_id); chat_message != nullptr)
    {
        chat_message->status = ChatMessageStatus::Sending;
        chat_message->failure_reason.clear();
    }
    else
    {
        // 防御分支：即使调用方未提前保存，也以完整模型补建本地记录。
        m_conversations[message.to].append(message);

        ensureConversationItem(message.to);

        moveConversationItemToTop(message.to);

        refreshConversationItem(message.to);
    }

    if (m_currentPeer == message.to)
    {
        renderCurrentConversation();
    }

    statusBar()->showMessage(QStringLiteral("消息发送中..."));
}
// 功能：将已被服务器接受的消息改为成功状态，并移除其待确认记录。
void MainWindow::onChatMessageAccepted(const ChatMessage &update)
{
    ChatMessage *message = findMessageByLocalId(update.local_id);
    if (message == nullptr)
    {
        statusBar()->showMessage(QStringLiteral("收到未知消息确认"));
        return;
    }

    const QString peer = message->to;

    message->message_id = update.message_id;
    message->status = update.status;
    message->failure_reason = update.failure_reason;
    message->server_received_at_ms = update.server_received_at_ms;

    sortConversationMessages(peer);

    if (m_currentPeer == peer)
    {
        renderCurrentConversation();
    }
    statusBar()->showMessage(QStringLiteral("服务器已接收消息"));
}
// 功能：将 local_id 对应待确认消息标记为失败，显示原因并允许用户重试。
void MainWindow::onChatSendFailed(const ChatMessage &update)
{
    ChatMessage *message = findMessageByLocalId(update.local_id);
    if (message == nullptr)
    {
        statusBar()->showMessage(QStringLiteral("未知消息发送失败：") + update.failure_reason);
        return;
    }
    message->status = update.status;
    message->failure_reason = update.failure_reason;

    if (m_currentPeer == message->to)
    {
        renderCurrentConversation();
    }

    statusBar()->showMessage(QStringLiteral("消息发送失败：") + update.failure_reason);
}

// 功能：将已存在的本地消息标记为最终送达，不创建新气泡或重排会话。
void MainWindow::onChatMessageDelivered(const ChatMessage &update)
{
    ChatMessage *message = findMessageByLocalId(update.local_id);
    if (message == nullptr)
    {
        statusBar()->showMessage(QStringLiteral("收到未知送达回执"));
        return;
    }
    message->status = update.status;
    message->failure_reason = update.failure_reason;
    if (m_currentPeer == message->to)
    {
        renderCurrentConversation();
    }
    statusBar()->showMessage(QStringLiteral("消息已送达"));
}

// ==================== 模块：接收消息处理 ====================
// 功能：将服务端转发的消息渲染为收到状态的聊天气泡。
void MainWindow::onChatMessageReceived(const ChatMessage &message)
{
    const QString peer = message.from;
    // 追加实时消息
    m_conversations[peer].append(message);
    // 排序模型
    sortConversationMessages(peer);
    // 确保会话列表项存在
    ensureConversationItem(peer);
    // 移动会话列表项到顶部
    moveConversationItemToTop(peer);

    if (m_currentPeer == peer)
    {
        renderCurrentConversation();
    }
    else
    {
        m_unreadCounts[peer]++;
    }
    refreshConversationItem(peer);
    m_chat->sendDeliveryReceipt(message.message_id);
}

void MainWindow::onHistoryPageReceived(const QList<ChatMessage> &messages, bool has_more)
{
    Q_UNUSED(has_more);

    QSet<QString> known_message_ids; // 用于去重
    QSet<QString> changed_peers;     // 用于记录哪些会话需要刷新

    // 1.把现有模型中的ID收集,cbegin()/cend() 是“只读遍历”
    for (auto conversation_it = m_conversations.cbegin(); conversation_it != m_conversations.cend(); ++conversation_it)
    {
        for (const ChatMessage &existing_message : conversation_it.value())
        {
            known_message_ids.insert(existing_message.message_id);
        }
    }
    // 2.逐条合并历史页
    for (const auto &message : messages)
    {
        if (known_message_ids.contains(message.message_id))
        {
            continue;
        }
        ChatMessage history_message = message;
        QString peer;
        // 3.计算peer
        if (history_message.from == m_username)
        {
            peer = history_message.to;
            history_message.status = ChatMessageStatus::Accepted;
        }
        else if (history_message.to == m_username)
        {
            peer = history_message.from;
        }
        else
        {
            continue;
        }
        // 4.真正提交模型，并立刻登记 ID
        m_conversations[peer].append(history_message);
        known_message_ids.insert(history_message.message_id);
        changed_peers.insert(peer);
    }
    // 5.循环结束后才刷新 UI
    for (const auto &peer : changed_peers)
    {
        sortConversationMessages(peer);
        ensureConversationItem(peer);
        refreshConversationItem(peer);
    }
    if (changed_peers.contains(m_currentPeer))
    {
        renderCurrentConversation();
    }
}

// ==================== 模块：窗口初始化与连接状态辅助 ====================
// 功能：设置窗口大小、当前用户名、默认会话提示和初始发送按钮状态。
void MainWindow::setupUiState()
{
    setWindowTitle(QStringLiteral("ChatHub"));
    setMinimumSize(960, 640);
    resize(1120, 720);
    ui->currentUserLabel->setText(QStringLiteral("当前用户：") + m_username);
    ui->peerNameLabel->setText(QStringLiteral("选择一个会话开始聊天"));
    ui->sendBtn->setEnabled(false);
}
// 功能：连接界面控件和 ChatClient 信号，使窗口随连接和消息状态自动更新。
void MainWindow::connectSlots()
{
    connect(m_chat, &ChatClient::disconnected, this, &MainWindow::onDisconnected);
    connect(ui->sendBtn, &QPushButton::clicked, this, &MainWindow::onSendClicked);
    connect(m_chat, &ChatClient::chatMessageQueued, this, &MainWindow::onChatMessageQueued);
    connect(m_chat, &ChatClient::chatMessageAccepted, this, &MainWindow::onChatMessageAccepted);
    connect(m_chat, &ChatClient::chatSendFailed, this, &MainWindow::onChatSendFailed);
    connect(m_chat, &ChatClient::chatMessageDelivered, this, &MainWindow::onChatMessageDelivered);
    connect(m_chat, &ChatClient::chatMessageReceived, this, &MainWindow::onChatMessageReceived);
    connect(m_chat, &ChatClient::onlineUsersChanged, this, &MainWindow::updateOnlineUsers);
    connect(m_chat, &ChatClient::historyPageReceived, this, &MainWindow::onHistoryPageReceived);

    connect(ui->conversationList, &QListWidget::itemClicked, this, &MainWindow::onConversationItemClicked);
    connect(ui->onlineUsersList, &QListWidget::itemClicked, this, &MainWindow::onOnlineUserItemClicked);

    connect(m_chat, &ChatClient::authSucceeded, this, [this] {
        // 功能：认证成功后将状态标签和发送按钮切换为可用状态
        updateConnectionState(true, QStringLiteral("连接正常"));
    });
    connect(m_chat, &ChatClient::serverError, this,
            [this](const QString &reason) { statusBar()->showMessage(QStringLiteral("服务器错误：") + reason); });

    updateConnectionState(m_chat->isAuthenticated(),
                          m_chat->isAuthenticated() ? QStringLiteral("连接正常") : QStringLiteral("连接未认证"));
}
// 功能：根据连接状态更新状态标签的样式和文本，并同步控制发送按钮。
void MainWindow::updateConnectionState(const bool connected, const QString &message) const
{
    ui->connectionStateLabel->setProperty("status", connected ? "ok" : "error");
    ui->connectionStateLabel->style()->unpolish(ui->connectionStateLabel);
    ui->connectionStateLabel->style()->polish(ui->connectionStateLabel);
    ui->connectionStateLabel->setText(message);
    ui->sendBtn->setEnabled(connected);
}
// 功能：创建一个本地 ChatMessage 模型，用于发送到服务器。
ChatMessage MainWindow::makeOutgoingChatMessage(const QString &to, const QString &content) const
{
    ChatMessage message;
    message.local_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    message.from = m_username;
    message.to = to;
    message.content = content;
    message.send_at = QDateTime::currentDateTimeUtc();
    message.status = ChatMessageStatus::Sending;
    message.failure_reason.clear();

    return message;
}

//==================== 模块：会话与气泡辅助 ====================
// 功能：确保会话列表中存在指定联系人，不存在则添加。
void MainWindow::ensureConversationItem(const QString &peer)
{
    for (int i = 0; i < ui->conversationList->count(); ++i)
    {
        const auto item = ui->conversationList->item(i);
        if (peer == item->data(Qt::UserRole).toString())
        {
            return;
        }
    }
    const auto item = new QListWidgetItem(peer, ui->conversationList);
    item->setData(Qt::UserRole, peer);
}
// 功能：清空消息气泡布局。
void MainWindow::clearMessageBubbles() const
{
    while (ui->messageLayout->count() > 1)
    {
        const QLayoutItem *item = ui->messageLayout->takeAt(1);
        if (item == nullptr)
        {
            break;
        }

        delete item->widget();
        delete item;
    }
}
// 功能：根据当前会话记录清空并重新创建消息气泡
void MainWindow::renderCurrentConversation()
{
    clearMessageBubbles();

    if (m_currentPeer.isEmpty())
    {
        return;
    }

    // 使用 constFind() 读取 QHash：它不会像 operator[] 一样在键不存在时意外创建空会话。
    const auto chatMessage_it = m_conversations.constFind(m_currentPeer);

    if (chatMessage_it == m_conversations.end())
    {
        return;
    }

    for (const auto &message : chatMessage_it.value())
    {
        appendMessageBubble(message);
    }
}

void MainWindow::sortConversationMessages(const QString &peer)
{
    if (peer.isEmpty())
    {
        return;
    }
    auto conversation_it = m_conversations.find(peer);
    if (conversation_it == m_conversations.end())
    {
        return;
    }

    QList<ChatMessage> &messages = conversation_it.value();
    std::stable_sort(messages.begin(), messages.end(), messageComesBefore);
}

// 功能：向消息气泡布局中添加一个新气泡。
void MainWindow::appendMessageBubble(const ChatMessage &message)
{

    auto *row = new QWidget(ui->messageContainer);
    auto *row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);

    auto *label = new QLabel(row);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QString text = message.from + QStringLiteral(": ") + message.content;
    if (message.send_at.isValid())
    {
        text = message.from + QStringLiteral(": ") + message.content + QStringLiteral("\n") + QStringLiteral(" [") +
               formatConversationTime(message.send_at, QDateTime::currentDateTimeUtc()) + QStringLiteral("]");
    }

    if (message.status == ChatMessageStatus::Delivered && message.from == m_username)
    {
        text += QStringLiteral("\n [已送达]");
    }

    label->setText(text);

    label->setProperty("local_id", message.local_id);
    label->setProperty("from", message.from);
    label->setProperty("to", message.to);
    const QString status_text = chatMessageStatusToString(message.status);
    label->setProperty("status", status_text);

    auto *retry_button = new QToolButton(row);
    connect(retry_button, &QToolButton::clicked, this,
            // 功能：用户点击失败标记时，重试该按钮所属 local_id 的消息。
            [this, local_id = message.local_id] { onRetryClicked(local_id); });
    retry_button->setText(QStringLiteral("!"));
    retry_button->setFixedSize(18, 18);

    const bool is_failed = message.status == ChatMessageStatus::Failed;
    retry_button->setVisible(is_failed);
    retry_button->setToolTip(message.failure_reason.isEmpty() ? QStringLiteral("消息发送失败")
                                                              : message.failure_reason);

    retry_button->setStyleSheet(QStringLiteral("QToolButton {"
                                               "color: white;"
                                               "background-color: #e53935;"
                                               "border: none;"
                                               "border-radius: 9px;"
                                               "font-weight: bold;"
                                               "}"));

    const bool is_mine = message.from == m_username;
    const Qt::Alignment alignment = is_mine ? Qt::AlignRight : Qt::AlignLeft;
    row_layout->addWidget(label, 1);
    row_layout->addWidget(retry_button, 0, Qt::AlignCenter);

    ui->messageLayout->addWidget(row, 0, alignment);
    ui->messageScrollArea->ensureWidgetVisible(row);
}
// 功能：刷新会话列表中指定联系人的显示内容。
void MainWindow::refreshConversationItem(const QString &peer)
{
    if (peer.isEmpty())
    {
        return;
    }
    for (int i = 0; i < ui->conversationList->count(); ++i)
    {
        const auto item = ui->conversationList->item(i);
        if (item->data(Qt::UserRole).toString() == peer)
        {
            // ========== 1. 构造第一行标题（名称 + 未读数 + 时间摘要） ==========
            const int unread_count = m_unreadCounts.value(peer, 0);
            const auto preview = makeConversationPreview(peer);
            const auto time_text = makeConversationTimeText(peer);
            QString line1;
            if (unread_count == 0)
            {
                line1 = QStringLiteral("%1").arg(peer);
            }
            else
            {
                line1 = QStringLiteral("%1 (%2)").arg(peer).arg(unread_count);
            }
            // 有时间文本才追加分隔符 · 时间
            if (!time_text.isEmpty())
            {
                line1 += QStringLiteral(" \u00B7 %1").arg(time_text);
            }

            // ========== 2. 换行拼接，一次性 setText ==========
            item->setText(line1 + QStringLiteral("\n") + preview);
        }
    }
}
// 功能：将指定联系人的未读消息计数标记为已读。
void MainWindow::markConversationRead(const QString &peer)
{
    if (peer.isEmpty())
    {
        return;
    }
    m_unreadCounts.remove(peer);
    refreshConversationItem(peer);
}

// 功能：生成会话预览文本。
QString MainWindow::makeConversationPreview(const QString &peer) const
{
    if (const auto chatMessage_it = m_conversations.constFind(peer); chatMessage_it != m_conversations.end())
    {
        if (const auto &messages = chatMessage_it.value(); !messages.isEmpty())
        {
            const auto &message = messages.last();
            QString preview = message.content;
            preview.replace(QStringLiteral("\r\n"), QStringLiteral(" "));
            preview.replace(u'\n', u' ');
            preview.replace(u'\r', u' ');

            if (preview.size() > kConversationPreviewMaxLength)
            {
                // 1. 先截取前缀文本
                preview = preview.left(kConversationPreviewMaxLength - 1);
                // 2. 判断末尾是否是高代理项 high surrogate
                // emoji 边界处理
                if (const char16_t last_char = preview.back().unicode(); QChar::isHighSurrogate(last_char))
                {
                    // 剔除残缺的高代理字符
                    preview.chop(1);
                }
                return preview + QStringLiteral("…");
            }
            return preview;
        }
    }
    return QStringLiteral("暂无消息");
}

QString MainWindow::formatConversationTime(const QDateTime &send_at, const QDateTime &now)
{
    if (!send_at.isValid() || !now.isValid())
    {
        return "";
    }
    // 2. 统一转为本地日期对比
    const QDateTime localSend = send_at.toLocalTime();
    const QDateTime localNow = now.toLocalTime();

    const QDate sendDate = localSend.date();
    const QDate today = localNow.date();

    // 今天
    if (sendDate == today)
    {
        return localSend.toString("HH:mm");
    }
    // 昨天
    if (sendDate == today.addDays(-1))
    {
        return QStringLiteral("昨天");
    }
    // 同年不同天：MM-dd
    if (sendDate.year() == today.year())
    {
        return localSend.toString("MM-dd");
    }
    // 跨年
    else
    {
        return localSend.toString("yyyy-MM-dd");
    }
}

QString MainWindow::makeConversationTimeText(const QString &peer) const
{
    if (peer.isEmpty())
    {
        return "";
    }

    if (const auto chatMessage_it = m_conversations.constFind(peer); chatMessage_it != m_conversations.end())
    {
        if (const auto &messages = chatMessage_it.value(); !messages.isEmpty())
        {
            const auto &message = messages.last();
            const QDateTime now = QDateTime::currentDateTimeUtc();

            return formatConversationTime(message.send_at, now);
        }
    }
    return "";
}

void MainWindow::moveConversationItemToTop(const QString &peer)
{
    if (peer.isEmpty())
    {
        return;
    }

    for (int i = 0; i < ui->conversationList->count(); ++i)
    {
        if (ui->conversationList->item(i)->data(Qt::UserRole).toString() == peer)
        {
            const auto item = ui->conversationList->takeItem(i);
            ui->conversationList->insertItem(0, item);
            return;
        }
    }
}

// 功能：整体替换在线用户视图，过滤当前用户后重新创建列表项。
void MainWindow::updateOnlineUsers(const QStringList &users)
{
    m_onlineUsers = users;
    ui->onlineUsersList->clear();
    for (const auto &username : m_onlineUsers)
    {
        if (username == m_username)
        {
            continue;
        }
        auto *item = new QListWidgetItem(username);
        item->setData(Qt::UserRole, username);
        ui->onlineUsersList->addItem(item);
    }
    ui->onlineUsersList->setEnabled(ui->onlineUsersList->count() > 0);
}

// ==================== 模块：消息查询辅助 ====================
// 功能：将消息状态枚举转换为字符串。
QString MainWindow::chatMessageStatusToString(const ChatMessageStatus status)
{
    switch (status)
    {
    case ChatMessageStatus::Accepted:
        return QStringLiteral("accepted");
    case ChatMessageStatus::Failed:
        return QStringLiteral("failed");
    case ChatMessageStatus::Received:
        return QStringLiteral("received");
    case ChatMessageStatus::Sending:
        return QStringLiteral("sending");
    case ChatMessageStatus::Delivered:
        return QStringLiteral("delivered");
    default:
        return QStringLiteral("unknown");
    }
}
// 功能：根据 local_id 查找消息。
ChatMessage *MainWindow::findMessageByLocalId(const QString &local_id)
{

    if (local_id.isEmpty())
    {
        return nullptr;
    }

    for (auto conversation_it = m_conversations.begin(); conversation_it != m_conversations.end(); ++conversation_it)
    {
        for (ChatMessage &message : conversation_it.value())
        {
            if (message.local_id == local_id)
            {
                return &message;
            }
        }
    }
    return nullptr;
}
