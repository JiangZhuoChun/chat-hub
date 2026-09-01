#include <boost/json.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

// 为了直接验证 Server 内部的历史响应序列化逻辑，本测试编译同一份实现代码。
#include "../src/net/server.cpp"

namespace
{

// 功能：构造一条用于验证协议字段映射的 Repository 历史记录。
repository::StoredMessage makeStoredMessage()
{
    repository::StoredMessage message;
    message.message_id = "message-1";
    message.sender = "alice";
    message.recipient = "bob";
    message.content = "hello";
    message.client_send_at = "2026-08-16T10:00:00.000Z";
    message.client_local_id = "local-1";
    message.server_received_at_ms = 1723456789000;
    return message;
}

// 功能：验证最终历史响应保留字段映射、分页状态和复合游标。
bool testFinalHistoryResponseBody()
{
    try
    {
        boost::json::array messages;
        messages.emplace_back(makeHistoryMessageObject(makeStoredMessage()));

        repository::HistoryQueryResult query_result;
        query_result.has_more = true;
        query_result.next_cursor = repository::HistoryCursor{1723456789000, "message-1"};

        const auto body = makeHistoryResultBody("request-1", messages, true, query_result);
        const auto object = boost::json::parse(body).as_object();
        const auto &response_messages = object.at("messages").as_array();
        const auto &message = response_messages.at(0).as_object();
        const auto &cursor = object.at("next_cursor").as_object();

        return object.at("request_id").as_string() == "request-1" && object.at("is_last_chunk").as_bool() &&
               object.at("has_more").as_bool() && response_messages.size() == 1 &&
               message.at("message_id").as_string() == "message-1" && message.at("local_id").as_string() == "local-1" &&
               message.at("from").as_string() == "alice" && message.at("to").as_string() == "bob" &&
               message.at("send_at").as_string() == "2026-08-16T10:00:00.000Z" &&
               message.at("server_received_at_ms").as_int64() == 1723456789000 &&
               cursor.at("server_received_at_ms").as_int64() == 1723456789000 &&
               cursor.at("message_id").as_string() == "message-1";
    }
    catch (const std::exception &error)
    {
        std::cerr << "最终历史响应解析失败：" << error.what() << '\n';
        return false;
    }
}

// 功能：验证非最终块不提前声明分页结论，空查询仍返回完整最终响应。
bool testIntermediateAndEmptyHistoryResponseBody()
{
    try
    {
        repository::HistoryQueryResult page_with_more;
        page_with_more.has_more = true;
        page_with_more.next_cursor = repository::HistoryCursor{1, "cursor-1"};

        boost::json::array messages;
        messages.emplace_back(makeHistoryMessageObject(makeStoredMessage()));
        const auto intermediate =
            boost::json::parse(makeHistoryResultBody("request-2", messages, false, page_with_more)).as_object();

        repository::HistoryQueryResult empty_result;
        const auto empty =
            boost::json::parse(makeHistoryResultBody("request-3", boost::json::array{}, true, empty_result))
                .as_object();

        return !intermediate.if_contains("has_more") && !intermediate.if_contains("next_cursor") &&
               !intermediate.at("is_last_chunk").as_bool() && empty.at("messages").as_array().empty() &&
               empty.at("is_last_chunk").as_bool() && !empty.at("has_more").as_bool() &&
               empty.at("next_cursor").is_null();
    }
    catch (const std::exception &error)
    {
        std::cerr << "中间或空历史响应解析失败：" << error.what() << '\n';
        return false;
    }
}

// 功能：验证帧大小判断必须基于实际序列化后的 JSON body 字节数。
bool testSerializedHistoryResponseCanExceedBodyLimit()
{
    auto oversized = makeStoredMessage();
    oversized.content.assign(protocol::kMaxFrameBodyLength, 'x');

    boost::json::array messages;
    messages.emplace_back(makeHistoryMessageObject(oversized));

    const repository::HistoryQueryResult result;
    const auto body = makeHistoryResultBody("request-4", messages, true, result);
    return body.size() > protocol::kMaxFrameBodyLength;
}

// 功能：输出单个测试结论，并聚合进程退出状态。
bool runTest(const char *name, const bool passed)
{
    if (passed)
    {
        std::cout << "PASS: " << name << '\n';
        return true;
    }

    std::cerr << "FAIL: " << name << '\n';
    return false;
}

} // namespace

int main()
{
    const bool all_passed =
        runTest("final history response body", testFinalHistoryResponseBody()) &&
        runTest("intermediate and empty history response body", testIntermediateAndEmptyHistoryResponseBody()) &&
        runTest("serialized history response byte count", testSerializedHistoryResponseCanExceedBodyLimit());
    return all_passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
