#include "protocol/chat_payload.h"

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cctype>
#include <limits>

#include "protocol/chat_protocol.h"

namespace
{

constexpr std::size_t kMaxLocalIdLength = 64;
constexpr std::size_t kMaxDeliveryReceiptMessageIdLength = 64;
constexpr std::size_t kMaxSendAtLength = 64;
constexpr std::size_t kMaxHistoryRequestIdLength = 64;
constexpr std::size_t kMaxHistoryCursorMessageIdLength = 64;
constexpr std::int64_t kMinHistoryLimit = 1;
constexpr std::int64_t kMaxHistoryLimit = 50;

bool isBlank(const std::string_view text)
{
    // 逐个字符检查，只有全为空白字符时才返回 true。
    return std::all_of(text.begin(), text.end(), [](const unsigned char character) { return std::isspace(character); });
}

bool tryNormalizeHistoryLimit(const boost::json::value &value, int &out_limit)
{
    // 先处理有符号整数，允许负数并将其钳制为最小页大小。
    if (value.is_int64())
    {
        const auto requested = value.as_int64();
        out_limit = static_cast<int>(std::clamp(requested, kMinHistoryLimit, kMaxHistoryLimit));
        return true;
    }

    // 再处理无符号整数，避免把其直接转换为有符号整数造成溢出。
    if (value.is_uint64())
    {
        const auto requested = value.as_uint64();
        if (requested < static_cast<std::uint64_t>(kMinHistoryLimit))
        {
            out_limit = static_cast<int>(kMinHistoryLimit);
        }
        else if (requested > static_cast<std::uint64_t>(kMaxHistoryLimit))
        {
            out_limit = static_cast<int>(kMaxHistoryLimit);
        }
        else
        {
            out_limit = static_cast<int>(requested);
        }
        return true;
    }

    // 浮点数、字符串、布尔值和 null 都不是合法页大小。
    return false;
}

} // namespace

namespace protocol
{

// ==================== 聊天 JSON 正文校验 ====================
ChatPayloadResult parseChatPayload(const std::string_view body)
{
    // 第 1 步：将帧正文解析为 JSON 根值，并确认根节点是对象。
    boost::system::error_code error;
    const auto value = boost::json::parse(boost::json::string_view(body.data(), body.size()), error);

    if (error || !value.is_object())
    {
        return {ChatPayloadError::invalid_json, {}};
    }

    // 第 2 步：拒绝客户端伪造的发送者身份字段。
    const auto &object = value.as_object();
    if (object.if_contains("sender_id"))
    {
        return {ChatPayloadError::forbidden_sender_id, {}};
    }

    // 第 3 步：校验接收者字段。
    const auto *to = object.if_contains("to");
    if (!to)
    {
        return {ChatPayloadError::missing_recipient, {}};
    }
    if (!to->is_string())
    {
        return {ChatPayloadError::recipient_not_string, {}};
    }
    const auto &json_to = to->as_string();
    const std::string to_str(json_to.data(), json_to.size());
    if (isBlank(to_str))
    {
        return {ChatPayloadError::blank_recipient, {}};
    }

    // 第 4 步：校验客户端本地消息标识，用于 ack、失败提示和重试关联。
    const auto *local_id = object.if_contains("local_id");
    if (!local_id)
    {
        return {ChatPayloadError::missing_local_id, {}};
    }
    if (!local_id->is_string())
    {
        return {ChatPayloadError::local_id_not_string, {}};
    }
    const auto &json_local_id = local_id->as_string();
    const std::string local_id_str(json_local_id.data(), json_local_id.size());
    if (isBlank(local_id_str))
    {
        return {ChatPayloadError::blank_local_id, {}};
    }
    if (local_id_str.size() > kMaxLocalIdLength)
    {
        return {ChatPayloadError::local_id_too_long, {}};
    }

    // 第 5 步：校验客户端提交的发送时间文本。
    const auto *send_at = object.if_contains("send_at");
    if (!send_at)
    {
        return {ChatPayloadError::missing_send_at, {}, {}, local_id_str};
    }
    if (!send_at->is_string())
    {
        return {ChatPayloadError::send_at_not_string, {}, {}, local_id_str};
    }
    const auto &json_send_at = send_at->as_string();
    const std::string send_at_str(json_send_at.data(), json_send_at.size());
    if (isBlank(send_at_str))
    {
        return {ChatPayloadError::blank_send_at, {}, {}, local_id_str};
    }
    if (send_at_str.size() > kMaxSendAtLength)
    {
        return {ChatPayloadError::send_at_too_long, {}, {}, local_id_str};
    }

    // 第 6 步：校验聊天正文。
    const auto *content = object.if_contains("content");
    if (!content)
    {
        return {ChatPayloadError::missing_content, {}};
    }
    if (!content->is_string())
    {
        return {ChatPayloadError::content_not_string, {}};
    }
    const auto &json_content = content->as_string();
    const std::string content_str(json_content.data(), json_content.size());
    if (isBlank(content_str))
    {
        return {ChatPayloadError::blank_content, {}};
    }
    if (content_str.size() > protocol::kMaxChatContentBytes)
    {
        return {ChatPayloadError::content_too_long, {}};
    }

    // 第 7 步：仅返回全部校验通过后的可路由字段。
    return {ChatPayloadError::none, to_str, content_str, local_id_str, send_at_str};
}

// ==================== 送达回执 JSON 正文校验 ====================
DeliveryReceiptPayloadResult parseDeliveryReceiptPayload(const std::string_view body)
{
    // 第 1 步：解析 JSON，并要求根节点为对象。
    boost::system::error_code error;
    const auto value = boost::json::parse(boost::json::string_view(body.data(), body.size()), error);

    if (error || !value.is_object())
    {
        return {DeliveryReceiptPayloadError::invalid_json, {}};
    }

    // 第 2 步：校验回执关联的服务端消息标识。
    const auto *message_id = value.as_object().if_contains("message_id");
    if (!message_id)
    {
        return {DeliveryReceiptPayloadError::missing_message_id, {}};
    }
    if (!message_id->is_string())
    {
        return {DeliveryReceiptPayloadError::message_id_not_string, {}};
    }
    const auto &json_message_id = message_id->as_string();
    const std::string message_id_str(json_message_id.data(), json_message_id.size());
    if (isBlank(message_id_str))
    {
        return {DeliveryReceiptPayloadError::blank_message_id, {}};
    }
    if (message_id_str.size() > kMaxDeliveryReceiptMessageIdLength)
    {
        return {DeliveryReceiptPayloadError::message_id_too_long, {}};
    }

    // 第 3 步：返回已校验的消息标识，供待送达索引查询使用。
    return {DeliveryReceiptPayloadError::none, message_id_str};
}

// ==================== 历史查询 JSON 正文校验 ====================
HistoryQueryPayloadResult parseHistoryQueryPayload(const std::string_view body)
{
    // 第 1 步：解析 JSON，并要求根节点为对象。
    boost::system::error_code error;
    const auto value = boost::json::parse(boost::json::string_view(body.data(), body.size()), error);

    if (error || !value.is_object())
    {
        return {HistoryQueryPayloadError::invalid_json};
    }

    // 第 2 步：拒绝请求正文指定用户身份，身份只能来自认证 Session。
    const auto &object = value.as_object();
    if (object.if_contains("sender") || object.if_contains("recipient") || object.if_contains("username"))
    {
        return {HistoryQueryPayloadError::forbidden_identity_field};
    }

    // 第 3 步：校验本次历史请求与响应分块的关联标识。
    const auto *request_id = object.if_contains("request_id");
    if (!request_id)
    {
        return {HistoryQueryPayloadError::missing_request_id};
    }
    if (!request_id->is_string())
    {
        return {HistoryQueryPayloadError::request_id_not_string};
    }
    const auto &json_request_id = request_id->as_string();
    const std::string request_id_str(json_request_id.data(), json_request_id.size());
    if (isBlank(request_id_str))
    {
        return {HistoryQueryPayloadError::blank_request_id};
    }
    if (request_id_str.size() > kMaxHistoryRequestIdLength)
    {
        return {HistoryQueryPayloadError::request_id_too_long};
    }

    // 第 4 步：校验整数类型并将页大小钳制到协议范围 1..50。
    const auto *limit = object.if_contains("limit");
    if (!limit)
    {
        return {HistoryQueryPayloadError::missing_limit};
    }
    int effective_limit = 0;
    if (!tryNormalizeHistoryLimit(*limit, effective_limit))
    {
        return {HistoryQueryPayloadError::limit_not_integer};
    }

    // 第 5 步：校验可选游标；缺失 before 表示首屏请求。
    std::optional<HistoryQueryCursor> before_cursor = std::nullopt;
    const auto *before_value = object.if_contains("before");
    if (before_value)
    {
        if (!before_value->is_object())
        {
            return {HistoryQueryPayloadError::before_not_object};
        }

        // 第 5.1 步：校验游标时间戳，要求可安全表示为非负 int64。
        const auto &before_object = before_value->as_object();
        const auto *timestamp = before_object.if_contains("server_received_at_ms");
        if (!timestamp)
        {
            return {HistoryQueryPayloadError::missing_before_timestamp};
        }

        std::int64_t before_timestamp{};
        if (timestamp->is_int64())
        {
            if (timestamp->as_int64() < 0)
            {
                return {HistoryQueryPayloadError::negative_before_timestamp};
            }
            before_timestamp = timestamp->as_int64();
        }
        else if (timestamp->is_uint64())
        {
            if (timestamp->as_uint64() > (std::numeric_limits<std::int64_t>::max)())
            {
                return {HistoryQueryPayloadError::before_timestamp_not_integer};
            }
            before_timestamp = static_cast<std::int64_t>(timestamp->as_uint64());
        }
        else
        {
            return {HistoryQueryPayloadError::before_timestamp_not_integer};
        }

        // 第 5.2 步：校验与时间戳共同构成稳定分页边界的 message_id。
        const auto *message_id = before_object.if_contains("message_id");
        if (!message_id)
        {
            return {HistoryQueryPayloadError::missing_before_message_id};
        }
        if (!message_id->is_string())
        {
            return {HistoryQueryPayloadError::before_message_id_not_string};
        }
        const auto &json_message_id = message_id->as_string();
        const std::string message_id_str(json_message_id.data(), json_message_id.size());
        if (isBlank(message_id_str))
        {
            return {HistoryQueryPayloadError::blank_before_message_id};
        }
        if (message_id_str.size() > kMaxHistoryCursorMessageIdLength)
        {
            return {HistoryQueryPayloadError::before_message_id_too_long};
        }

        // 第 5.3 步：两个字段都通过后，整体构造游标，避免出现半有效状态。
        before_cursor = HistoryQueryCursor{before_timestamp, message_id_str};
    }

    // 第 6 步：返回完整且已校验的历史查询参数。
    return {HistoryQueryPayloadError::none, request_id_str, effective_limit, before_cursor};
}

} // namespace protocol
