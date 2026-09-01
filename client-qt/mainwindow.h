#pragma once

#include "chat_types.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QString>
#include <QStringList>
class ChatClient;
class QWidget;
class QListWidgetItem;

namespace Ui
{
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

  public:
    // ==================== 模块：窗口生命周期 ====================
    // 功能：创建聊天主窗口，保存当前用户并连接 ChatClient 的业务信号。
    explicit MainWindow(ChatClient *chat_client, QString username, QWidget *parent = nullptr);

    // 功能：释放 Qt 设计器创建的主窗口界面对象。
    ~MainWindow() override;

  private slots:
    // 1.连接状态槽
    //  ==================== 模块：窗口与连接状态 ====================
    //  功能：聊天连接断开时更新状态栏和发送按钮状态。
    void onDisconnected();

    // 2.用户操作槽
    //  ==================== 模块：用户发送操作 ====================
    //  功能：手填接收者优先，否则回退当前会话；校验通过后只交给 ChatClient 发送一次。
    void onSendClicked();
    // 功能：使用原始 local_id、接收者和正文重新提交失败消息。
    void onRetryClicked(const QString &local_id);
    // 功能：点击会话列表项时更新当前会话。
    void onConversationItemClicked(const QListWidgetItem *item);

    // 功能：将点击的在线用户名回填到接收者输入框，不创建或切换会话。
    void onOnlineUserItemClicked(const QListWidgetItem *item);

    // 3.消息状态槽
    //  ==================== 模块：消息发送状态处理 ====================
    //  功能：为已写入发送缓冲区的消息创建或恢复待确认气泡。
    void onChatMessageQueued(const ChatMessage &message);
    // 功能：将服务端已接受的消息气泡更新为成功状态并移出待确认表。
    void onChatMessageAccepted(const ChatMessage &update);
    // 功能：将发送失败的消息气泡标记为失败，并显示可点击的重试按钮。
    void onChatSendFailed(const ChatMessage &update);

    // 功能：将本地已有消息更新为 Delivered 状态，不新增气泡或改变会话顺序。
    void onChatMessageDelivered(const ChatMessage &update);

    // 4.接收消息槽
    //  ==================== 模块：接收消息处理 ====================
    //  功能：将服务端转发的聊天消息渲染为收到消息气泡。
    void onChatMessageReceived(const ChatMessage &message);

    void onHistoryPageReceived(const QList<ChatMessage> &messages, bool has_more);

  private:
    // ==================== 模块：窗口初始化与连接状态辅助 ====================
    // 功能：设置窗口尺寸、当前用户、默认会话提示和发送按钮初始状态。
    void setupUiState();

    // 功能：将按钮和 ChatClient 信号连接到主窗口的状态更新处理函数。
    void connectSlots();

    // 功能：更新连接状态标签的文本、样式属性和发送按钮可用状态。
    void updateConnectionState(bool connected, const QString &message) const;

    // 功能：创建一个本地消息对象，用于发送消息。
    ChatMessage makeOutgoingChatMessage(const QString &to, const QString &content) const;

    //==================== 模块：会话与气泡辅助 ====================
    // 功能：确保会话列表中存在指定联系人的会话项，不存在则创建。
    void ensureConversationItem(const QString &peer);
    // 功能：清空所有消息气泡。
    void clearMessageBubbles() const;
    // 功能：渲染当前会话。
    void renderCurrentConversation();
    // 功能：按服务端持久化顺序整理指定会话；未确认的本地消息保持在末尾。
    void sortConversationMessages(const QString &peer);
    // 功能：创建带发送者、正文、时间、状态属性和重试按钮的聊天气泡。
    void appendMessageBubble(const ChatMessage &message);
    // 功能：刷新指定联系人的会话项，更新未读消息计数。
    void refreshConversationItem(const QString &peer);
    // 功能：将指定联系人的未读消息计数标记为已读。
    void markConversationRead(const QString &peer);
    // 功能：创建或更新会话列表项的预览内容。
    QString makeConversationPreview(const QString &peer) const;
    // 功能：将发送时间格式化为相对时间。
    static QString formatConversationTime(const QDateTime &send_at, const QDateTime &now);
    // 功能：获取指定 peer 的时间摘要
    QString makeConversationTimeText(const QString &peer) const;
    // 功能：将指定联系人的会话项移动到会话列表顶部。
    void moveConversationItemToTop(const QString &peer);

    // 功能：整体替换在线列表模型，过滤当前用户后重建在线联系人视图。
    void updateOnlineUsers(const QStringList &users);

    // ==================== 模块：消息查询、依赖与状态 ====================
    // 1.消息查询辅助
    // 功能：将消息状态转换为 QSS 使用的字符串属性。
    static QString chatMessageStatusToString(ChatMessageStatus status);
    // 功能：根据本地消息 ID 查找消息。
    ChatMessage *findMessageByLocalId(const QString &local_id);

    // 2.外部依赖与界面对象
    //  功能：保存 Qt 设计器生成的界面对象，由析构函数释放。
    Ui::MainWindow *ui;
    // 功能：保存应用注入的聊天客户端，不负责释放。
    ChatClient *m_chat;

    // 3.当前用户与会话状态
    //  功能：保存已认证用户，用于判定消息是本人发送还是对方发送。
    QString m_username;

    // 功能：保存主窗口当前显示模型对应的完整在线用户快照。
    QStringList m_onlineUsers;
    // 与某个联系人的全部消息---联系人 → 消息列表
    QHash<QString, QList<ChatMessage>> m_conversations;
    // 功能：当前右侧正在显示的会话联系人。
    QString m_currentPeer;
    // 功能：保存每个联系人的未读消息计数。
    QHash<QString, int> m_unreadCounts;
};
