#pragma once

#include <QDateTime>
#include <QString>
#include <QtGlobal>
#include <optional>
// ==================== 模块：聊天领域类型 ====================
// 功能：集中定义界面层使用的消息状态和完整聊天记录数据。
enum class ChatMessageStatus
{
    Sending,
    Accepted,
    Failed,
    Received,
    Delivered
};

// 功能：表示一条完整聊天记录，不包含任何 Qt 控件。
struct ChatMessage
{
    // 功能：保存服务端首次持久化后分配的全局消息身份；用于接收方回执与历史去重。
    QString message_id;
    // 功能：关联客户端本地消息、确认、失败和重试的稳定标识；不用于接收方回执。
    QString local_id;
    // 功能：保存消息发送者的认证用户名。
    QString from;
    // 功能：保存消息接收者的认证用户名。
    QString to;
    // 功能：保存消息正文，不包含协议帧头或 UI 格式文本。
    QString content;
    // 功能：保存消息发送时刻，统一以 UTC 传输并按需转本地时间显示。
    QDateTime send_at;
    // 服务端成功持久化后分配的权威排序时间；本地待发送或发送失败消息没有该值。
    std::optional<qint64> server_received_at_ms = std::nullopt;
    // 功能：保存消息在本地发送、服务端接收和最终送达链路中的当前状态。
    ChatMessageStatus status{ChatMessageStatus::Received};
    // 功能：保存发送失败原因，成功或接收消息时保持为空。
    QString failure_reason;
};
