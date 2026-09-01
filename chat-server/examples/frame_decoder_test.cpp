#include "protocol/frame_decoder.h"
#include "protocol/chat_payload.h"

#include <iostream>
#include <string>

// ==================== 模块：帧解码场景测试 ====================
// 功能：验证半包分两次到达时，解码器只在正文完整后回调一条聊天消息。
bool testEmptyChatFrame() {
    protocol::FrameDecoder decoder;
    std::vector<protocol::Message> received;

    const auto frame = protocol::makeFrame(protocol::MessageType::chat, "Hello");
    const auto on_message =
        // 功能：记录解码器回调的完整消息，供测试断言数量和类型。
        [&received](const protocol::Message& message) {
            received.push_back(message);
        };
    const auto first_result = decoder.append(frame.data(), 3, on_message);
    const bool first_passed = first_result == protocol::DecodeResult::ok && received.empty();
    const auto second_result =
        decoder.append(frame.data() + 3, frame.size() - 3, on_message);
    return first_passed && second_result == protocol::DecodeResult::ok &&
           received.size() == 1 && received[0].type == protocol::MessageType::chat;
}

// 功能：验证同一次读取包含两帧时，解码器可以依次回调两条聊天消息。
bool testStickyFrames() {
    protocol::FrameDecoder decoder;
    std::vector<protocol::Message> received;
    const auto first = protocol::makeFrame(protocol::MessageType::chat, "Hello");
    const auto second = protocol::makeFrame(protocol::MessageType::chat, "World");
    const std::string frame = first + second;
    const auto on_message =
        // 功能：记录粘包解析出的每条完整消息，供测试验证顺序和正文。
        [&received](const protocol::Message& message) {
            received.push_back(message);
        };
    const auto result = decoder.append(frame.data(), frame.size(), on_message);
    return result == protocol::DecodeResult::ok && received.size() == 2 &&
           received[0].type == protocol::MessageType::chat &&
           received[1].type == protocol::MessageType::chat &&
           received[0].body == "Hello" && received[1].body == "World";
}

// 功能：验证帧魔数被篡改后，解码器返回 invalid_magic 且不交付消息。
bool testInvalidMagic() {
    protocol::FrameDecoder decoder;
    std::vector<protocol::Message> received;
    auto frame = protocol::makeFrame(protocol::MessageType::chat, "Hello");
    frame[0] = '\x00';
    const auto result = decoder.append(
        frame.data(), frame.size(),
        // 功能：记录意外解码出的消息，确保非法魔数场景下回调不会发生。
        [&received](const protocol::Message& message) {
            received.push_back(message);
        });
    return result == protocol::DecodeResult::invalid_magic && received.empty();
}

// 功能：验证正文长度超过协议上限时，解码器返回 message_too_large 且不交付消息。
bool testMaxBodyLength() {
    protocol::FrameDecoder decoder;
    std::vector<protocol::Message> received;
    const std::string body(protocol::kMaxFrameBodyLength + 1, 'x');
    const auto frame = protocol::makeFrame(protocol::MessageType::chat, body);
    const auto result = decoder.append(
        frame.data(), frame.size(),
        // 功能：记录意外解码出的消息，确保超长正文场景下回调不会发生。
        [&received](const protocol::Message& message) {
            received.push_back(message);
        });
    return result == protocol::DecodeResult::message_too_large && received.empty();
}

// 功能：验证正文长度恰好等于协议上限时，解码器正常交付完整消息。
bool testMaxBodyLengthAccepted() {
    protocol::FrameDecoder decoder;
    std::vector<protocol::Message> received;
    const std::string body(protocol::kMaxFrameBodyLength, 'x');
    const auto frame = protocol::makeFrame(protocol::MessageType::chat, body);
    const auto result = decoder.append(
        frame.data(), frame.size(),
        [&received](const protocol::Message& message) {
            received.push_back(message);
        });
    return result == protocol::DecodeResult::ok && received.size() == 1 &&
           received[0].type == protocol::MessageType::chat && received[0].body == body;
}

// 功能：验证不同类型的收据载荷解析结果。
bool testDeliverReceiptPayload() {
    const auto invalid_json =
    protocol::parseDeliveryReceiptPayload(R"({"message_id":})");
    const auto missing_id =
        protocol::parseDeliveryReceiptPayload(R"({})");
    const auto numeric_id =
        protocol::parseDeliveryReceiptPayload(R"({"message_id":123})");
    const auto blank_id =
        protocol::parseDeliveryReceiptPayload(R"({"message_id":" \t\n"})");
    const std::string too_long_body =
        "{\"message_id\":\"" + std::string(65, 'x') + "\"}";
    const auto too_long_id =
        protocol::parseDeliveryReceiptPayload(too_long_body);

    const auto result =
    protocol::parseDeliveryReceiptPayload(R"({"message_id":"message-1"})");

    return result.error ==protocol::DeliveryReceiptPayloadError::none &&
           result.message_id == "message-1" &&
           invalid_json.error == protocol::DeliveryReceiptPayloadError::invalid_json &&
           missing_id.error == protocol::DeliveryReceiptPayloadError::missing_message_id &&
           numeric_id.error == protocol::DeliveryReceiptPayloadError::message_id_not_string &&
           blank_id.error == protocol::DeliveryReceiptPayloadError::blank_message_id &&
           too_long_id.error == protocol::DeliveryReceiptPayloadError::message_id_too_long;
}

// 功能：验证合法历史查询、可选游标和页大小钳制后的结果。
bool testHistoryQueryPayloadSuccess() {
    // 第 1 步：首屏请求不携带 before，结果必须保留空游标。
    const auto first_page = protocol::parseHistoryQueryPayload(
        R"({"request_id":"history-first","limit":50})");

    // 第 2 步：翻页请求必须保留完整的复合游标。
    const auto older_page = protocol::parseHistoryQueryPayload(
        R"({"request_id":"history-older","limit":2,"before":{"server_received_at_ms":1723456789000,"message_id":"message-42"}})");

    // 第 3 步：整数 limit 超出范围时按协议钳制，而不是拒绝请求。
    const auto lower_bound = protocol::parseHistoryQueryPayload(
        R"({"request_id":"history-lower","limit":0})");
    const auto upper_bound = protocol::parseHistoryQueryPayload(
        R"({"request_id":"history-upper","limit":51})");

    return first_page.error == protocol::HistoryQueryPayloadError::none &&
           first_page.request_id == "history-first" &&
           first_page.limit == 50 &&
           !first_page.before.has_value() &&
           older_page.error == protocol::HistoryQueryPayloadError::none &&
           older_page.request_id == "history-older" &&
           older_page.limit == 2 &&
           older_page.before.has_value() &&
           older_page.before->server_received_at_ms == 1723456789000 &&
           older_page.before->message_id == "message-42" &&
           lower_bound.error == protocol::HistoryQueryPayloadError::none &&
           lower_bound.limit == 1 &&
           upper_bound.error == protocol::HistoryQueryPayloadError::none &&
           upper_bound.limit == 50;
}

// 功能：验证历史查询在字段缺失、类型错误、越界和身份伪造时拒绝请求。
bool testHistoryQueryPayloadRejection() {
    // 第 1 步：构造 request_id 与 cursor message_id 的超长输入。
    const std::string too_long_request =
        "{\"request_id\":\"" + std::string(65, 'x') + "\",\"limit\":1}";
    const std::string too_long_cursor_id =
        "{\"request_id\":\"history\",\"limit\":1,\"before\":{"
        "\"server_received_at_ms\":1,\"message_id\":\"" +
        std::string(65, 'x') + "\"}}";

    // 第 2 步：分别触发每类协议错误。
    const auto invalid_json = protocol::parseHistoryQueryPayload(
        R"({"request_id":})");
    const auto missing_request_id = protocol::parseHistoryQueryPayload(
        R"({"limit":1})");
    const auto numeric_request_id = protocol::parseHistoryQueryPayload(
        R"({"request_id":1,"limit":1})");
    const auto blank_request_id = protocol::parseHistoryQueryPayload(
        R"({"request_id":"   ","limit":1})");
    const auto long_request_id = protocol::parseHistoryQueryPayload(too_long_request);
    const auto missing_limit = protocol::parseHistoryQueryPayload(
        R"({"request_id":"history"})");
    const auto decimal_limit = protocol::parseHistoryQueryPayload(
        R"({"request_id":"history","limit":1.5})");
    const auto forged_sender = protocol::parseHistoryQueryPayload(
        R"({"request_id":"history","limit":1,"sender":"alice"})");
    const auto before_not_object = protocol::parseHistoryQueryPayload(
        R"({"request_id":"history","limit":1,"before":[]})");
    const auto missing_timestamp = protocol::parseHistoryQueryPayload(
        R"({"request_id":"history","limit":1,"before":{}})");
    const auto text_timestamp = protocol::parseHistoryQueryPayload(
        R"({"request_id":"history","limit":1,"before":{"server_received_at_ms":"1","message_id":"m"}})");
    const auto negative_timestamp = protocol::parseHistoryQueryPayload(
        R"({"request_id":"history","limit":1,"before":{"server_received_at_ms":-1,"message_id":"m"}})");
    const auto oversized_timestamp = protocol::parseHistoryQueryPayload(
        R"({"request_id":"history","limit":1,"before":{"server_received_at_ms":9223372036854775808,"message_id":"m"}})");
    const auto missing_message_id = protocol::parseHistoryQueryPayload(
        R"({"request_id":"history","limit":1,"before":{"server_received_at_ms":1}})");
    const auto numeric_message_id = protocol::parseHistoryQueryPayload(
        R"({"request_id":"history","limit":1,"before":{"server_received_at_ms":1,"message_id":1}})");
    const auto blank_message_id = protocol::parseHistoryQueryPayload(
        R"({"request_id":"history","limit":1,"before":{"server_received_at_ms":1,"message_id":"   "}})");
    const auto long_message_id = protocol::parseHistoryQueryPayload(too_long_cursor_id);

    return invalid_json.error == protocol::HistoryQueryPayloadError::invalid_json &&
           missing_request_id.error == protocol::HistoryQueryPayloadError::missing_request_id &&
           numeric_request_id.error == protocol::HistoryQueryPayloadError::request_id_not_string &&
           blank_request_id.error == protocol::HistoryQueryPayloadError::blank_request_id &&
           long_request_id.error == protocol::HistoryQueryPayloadError::request_id_too_long &&
           missing_limit.error == protocol::HistoryQueryPayloadError::missing_limit &&
           decimal_limit.error == protocol::HistoryQueryPayloadError::limit_not_integer &&
           forged_sender.error == protocol::HistoryQueryPayloadError::forbidden_identity_field &&
           before_not_object.error == protocol::HistoryQueryPayloadError::before_not_object &&
           missing_timestamp.error == protocol::HistoryQueryPayloadError::missing_before_timestamp &&
           text_timestamp.error == protocol::HistoryQueryPayloadError::before_timestamp_not_integer &&
           negative_timestamp.error == protocol::HistoryQueryPayloadError::negative_before_timestamp &&
           oversized_timestamp.error == protocol::HistoryQueryPayloadError::before_timestamp_not_integer &&
           missing_message_id.error == protocol::HistoryQueryPayloadError::missing_before_message_id &&
           numeric_message_id.error == protocol::HistoryQueryPayloadError::before_message_id_not_string &&
           blank_message_id.error == protocol::HistoryQueryPayloadError::blank_before_message_id &&
           long_message_id.error == protocol::HistoryQueryPayloadError::before_message_id_too_long;

}


bool testUsernameContract() {
    using namespace protocol;
    if (!isValidUsername("abc")) {
        return false;
    }
    if (!isValidUsername("A_1")) {
        return false;
    }
    // 恰好 3 字节
    if (!isValidUsername("abc")) {
        return false;
    }
    // 恰好 20 字节
    if (!isValidUsername("abcdefghijklmnopqrst")) {
        return false;
    }
    // 非法：空
    if (isValidUsername("")) {
        return false;
    }
    // 非法：2 字节
    if (isValidUsername("ab")) {
        return false;
    }
    // 非法：21 字节
    if (isValidUsername("abcdefghijklmnopqrstu")) {
        return false;
    }
    // 非法字符：-
    if (isValidUsername("ab-c")) {
        return false;
    }
    // 空格不能 trim
    if (isValidUsername("ab c")) {
        return false;
    }
    // 任一非 ASCII 字节
    const std::string non_ascii{'a','b',static_cast<char>(0x80)};
    if (isValidUsername(non_ascii)){
        return false;
    }
    return true;
}


bool testChatContentLengthBoundary()
{
    using namespace protocol;

    const auto makeJson = [](std::string_view content) {
        return std::string{
            R"({"to":"Bob","local_id":"local-1","send_at":"123456","content":")"
        } + std::string{content} + R"("})";
    };

    {
        const std::string content(kMaxChatContentBytes, 'a');

        const auto result = parseChatPayload(makeJson(content));

        if (result.error != ChatPayloadError::none || result.content != content) {
            return false;
        }
    }

    {
        const std::string content(kMaxChatContentBytes + 1, 'a');

        const auto result = parseChatPayload(makeJson(content));

        if (result.error != ChatPayloadError::content_too_long) {
            return false;
        }
    }

    return true;
}
// ==================== 模块：测试结果汇总 ====================
// 功能：输出单个测试用例的通过或失败结果，并返回其布尔状态。
bool runTest(const char* name, const bool passed) {
    if (passed) {
        std::cout << "PASS: " << name << '\n';
        return true;
    }

    std::cerr << "FAIL: " << name << '\n';
    return false;
}

// 功能：依次执行全部帧解码场景，并聚合最终测试结果。
bool testAll() {
    bool all_pass = true;
    if (!runTest("empty chat frame", testEmptyChatFrame())) {
        all_pass = false;
    }
    if (!runTest("sticky frames", testStickyFrames())) {
        all_pass = false;
    }
    if (!runTest("invalid magic", testInvalidMagic())) {
        all_pass = false;
    }
    if (!runTest("max body length", testMaxBodyLength())) {
        all_pass = false;
    }
    if (!runTest("max body length accepted", testMaxBodyLengthAccepted())) {
        all_pass = false;
    }
    if (!runTest("delivery receipt payload", testDeliverReceiptPayload())) {
        all_pass = false;
    }
    if (!runTest("history query payload success", testHistoryQueryPayloadSuccess())) {
        all_pass = false;
    }
    if (!runTest("history query payload rejection", testHistoryQueryPayloadRejection())) {
        all_pass = false;
    }
    if (!runTest("username contract", testUsernameContract())) {
        all_pass = false;
    }
    if (!runTest("chat content length boundary", testChatContentLengthBoundary())) {
        all_pass = false;
    }
    return all_pass;
}

// ==================== 模块：帧解码测试入口 ====================
// 功能：执行全部帧解码测试，并按结果返回进程成功或失败状态。
int main() {
    if (const bool all_passed = testAll()) {
        std::cout << "PASS: split chat frame\n";
        return EXIT_SUCCESS;
    }

    std::cerr << "FAIL: split chat frame\n";
    return EXIT_FAILURE;
}
