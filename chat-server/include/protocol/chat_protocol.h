#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
namespace protocol
{

// ==================== 模块：聊天帧公共定义 ====================
// 功能：集中定义客户端和服务端都必须遵守的帧类型与固定帧头常量。
enum class MessageType : std::uint8_t
{
    chat = 1,
    ping = 2,
    pong = 3,
    error = 4,
    auth = 5,
    chat_ack = 6,
    delivery_receipt = 7,
    online_users = 8,
    history_query = 9,
    history_result = 10
};

// 功能：标识 ChatHub 协议帧，接收端据此拒绝非本协议字节流。
inline constexpr std::uint16_t kFrameMagic = 0x4348;
// 功能：标识当前帧格式版本，用于拒绝不兼容的协议帧。
inline constexpr std::uint8_t kProtocolVersion = 1;
// 功能：定义固定帧头的字节数，用于定位正文长度字段和正文起始位置。
inline constexpr std::size_t kFrameHeaderLength = 8;
// 单帧 body 总上限（容纳 content + JSON 字段开销）。
inline constexpr std::size_t kMaxFrameBodyLength = 2048;
// 纯文本 content 上限（客户端发送的正文 ≤ 此值）
inline constexpr std::size_t kMaxChatContentBytes = 1024;

inline constexpr std::size_t kMinUsernameBytes = 3;

inline constexpr std::size_t kMaxUsernameBytes = 20;

inline constexpr std::size_t kMaxOnlineUsersSnapshotCount = 88;
// 功能：判断帧头中的原始 type 值是否属于当前协议支持的类型范围。
constexpr bool isKnownMessageType(const std::uint8_t raw_type)
{
    return raw_type >= static_cast<std::uint8_t>(MessageType::chat) &&
           raw_type <= static_cast<std::uint8_t>(MessageType::history_result);
}

constexpr bool isValidUsername(const std::string_view username)
{
    // 长度必须在 3..20；
    if (username.size() < kMinUsernameBytes || username.size() > kMaxUsernameBytes)
    {
        return false;
    }
    // 每个字符只能是 A-Z、a-z、0-9 或 _；
    for (const char ch : username)
    {
        const bool valid =
            (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || (ch == '_');
        if (!valid)
        {
            return false;
        }
    }
    return true;
}
} // namespace protocol
