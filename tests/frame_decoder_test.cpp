// M1-4 帧层单元测试：半包、粘包、空正文、超长、非法 UTF-8、未知 type、版本不兼容、往返。

#include "chathub/contracts/frame.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace chathub::contracts;

#define CHECK(expr)                                          \
    do {                                                     \
        if (!(expr)) {                                       \
            std::fputs("CHECK failed: " #expr "\n", stderr); \
            return 1;                                        \
        }                                                    \
    } while (false)

static std::vector<std::uint8_t> encoded(ProtocolType type, std::string_view body)
{
    auto bytes = encode_frame(type, body);
    if (!bytes) {
        std::fputs("encode_frame unexpectedly failed\n", stderr);
        std::exit(1);
    }
    return *bytes;
}

static std::vector<std::uint8_t> make_header(
    std::uint8_t hi, std::uint8_t lo, std::uint8_t version,
    std::uint8_t type, std::uint32_t length)
{
    // 直接构造原始帧头，专门用于绕过 encode_frame 测试非法入站数据。
    return {hi, lo, version, type,
        static_cast<std::uint8_t>((length >> 24) & 0xFF),
        static_cast<std::uint8_t>((length >> 16) & 0xFF),
        static_cast<std::uint8_t>((length >> 8) & 0xFF),
        static_cast<std::uint8_t>(length & 0xFF)};
}

static int test_round_trip()
{
    FrameDecoder decoder;
    const auto bytes = encoded(ProtocolType::message_send_request, "{\"a\":1}");
    decoder.feed(bytes.data(), bytes.size());
    Frame frame;
    CHECK(decoder.next(frame));
    CHECK(frame.type == ProtocolType::message_send_request);
    CHECK(frame.body == "{\"a\":1}");
    CHECK(!decoder.next(frame));
    CHECK(!decoder.failed());

    const auto heartbeat = encoded(ProtocolType::heartbeat, "");  // 空正文（D72）
    decoder.feed(heartbeat.data(), heartbeat.size());
    CHECK(decoder.next(frame));
    CHECK(frame.type == ProtocolType::heartbeat);
    CHECK(frame.body.empty());
    return 0;
}

static int test_partial_bytes()
{
    FrameDecoder decoder;
    const auto bytes = encoded(ProtocolType::login_request, "{\"account\":\"abcd\"}");
    Frame frame;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const std::uint8_t byte = bytes[i];
        decoder.feed(&byte, 1);
        CHECK(!decoder.failed());
        // 半包阶段不能提前交付；只有最后一个字节补齐正文后才允许产出。
        if (i + 1 < bytes.size()) {
            CHECK(!decoder.next(frame));
        } else {
            CHECK(decoder.next(frame));
            CHECK(frame.type == ProtocolType::login_request);
            CHECK(frame.body == "{\"account\":\"abcd\"}");
            Frame empty;
            CHECK(!decoder.next(empty));
        }
    }
    return 0;
}

static int test_coalesced_frames()
{
    FrameDecoder decoder;
    const auto first = encoded(ProtocolType::heartbeat, "");
    const auto second = encoded(ProtocolType::contact_list_request, "[]");
    std::vector<std::uint8_t> both(first);
    both.insert(both.end(), second.begin(), second.end());
    decoder.feed(both.data(), both.size());
    Frame frame;
    CHECK(decoder.next(frame));
    CHECK(frame.type == ProtocolType::heartbeat);
    CHECK(frame.body.empty());
    CHECK(decoder.next(frame));
    CHECK(frame.type == ProtocolType::contact_list_request);
    CHECK(frame.body == "[]");
    CHECK(!decoder.next(frame));
    CHECK(!decoder.failed());
    return 0;
}

static int test_protocol_errors()
{
    {  // magic 错误：直接 failed，不重同步（D70）
        FrameDecoder decoder;
        const auto bad = make_header(0x00, 0x48, 1, 0x02, 0);
        decoder.feed(bad.data(), bad.size());
        CHECK(decoder.failed());
        CHECK(decoder.error() == FrameErrorKind::bad_magic);
    }
    {  // 版本不兼容（D70/D12）
        FrameDecoder decoder;
        const auto bad = make_header(0x43, 0x48, 2, 0x02, 0);
        decoder.feed(bad.data(), bad.size());
        CHECK(decoder.failed());
        CHECK(decoder.error() == FrameErrorKind::bad_version);
    }
    {  // 未知 type（预留 0x03）
        FrameDecoder decoder;
        const auto bad = make_header(0x43, 0x48, 1, 0x03, 0);
        decoder.feed(bad.data(), bad.size());
        CHECK(decoder.failed());
        CHECK(decoder.error() == FrameErrorKind::unknown_type);
    }
    {  // 4 GiB 超长：只喂帧头即拒绝，分配前生效（D09）
        FrameDecoder decoder;
        const auto bad = make_header(0x43, 0x48, 1, 0x50, 0xFFFFFFFF);
        decoder.feed(bad.data(), bad.size());
        CHECK(decoder.failed());
        CHECK(decoder.error() == FrameErrorKind::body_too_large);
    }
    {  // 超过该 type 上限：heartbeat max_body=0，带 1 字节正文拒绝
        FrameDecoder decoder;
        auto bad = make_header(0x43, 0x48, 1, 0x02, 1);
        bad.push_back('x');
        decoder.feed(bad.data(), bad.size());
        CHECK(decoder.failed());
        CHECK(decoder.error() == FrameErrorKind::body_too_large);
    }
    {  // 非法 UTF-8：0xED 0xA0 0x80 为代理区
        FrameDecoder decoder;
        const std::string invalid_utf8("a\xED\xA0\x80z");
        // 编码端已拒绝非法 UTF-8；此处必须手工组帧，才能验证入站拒绝路径。
        auto bad = make_header(0x43, 0x48, 1,
            static_cast<std::uint8_t>(ProtocolType::message_send_request),
            static_cast<std::uint32_t>(invalid_utf8.size()));
        bad.insert(bad.end(), invalid_utf8.begin(), invalid_utf8.end());
        decoder.feed(bad.data(), bad.size());
        CHECK(decoder.failed());
        CHECK(decoder.error() == FrameErrorKind::invalid_utf8);
    }
    {  // 合法 UTF-8（中文＋Emoji）不误伤
        FrameDecoder decoder;
        const auto ok = encoded(ProtocolType::message_send_request,
            "{\"text\":\"中文😀\"}");
        decoder.feed(ok.data(), ok.size());
        Frame frame;
        CHECK(decoder.next(frame));
        CHECK(!decoder.failed());
    }
    return 0;
}

static int test_failed_decoder_stops()
{
    FrameDecoder decoder;
    const auto bad = make_header(0x43, 0x48, 9, 0x02, 0);
    decoder.feed(bad.data(), bad.size());
    CHECK(decoder.failed());
    const auto good = encoded(ProtocolType::heartbeat, "");
    decoder.feed(good.data(), good.size());  // 失败后输入被忽略
    Frame frame;
    CHECK(!decoder.next(frame));
    CHECK(decoder.failed());
    return 0;
}

static int test_encode_rejects_invalid_input()
{
    // 出站校验与解码器的 type、长度、UTF-8 三项边界保持对称。
    CHECK(!encode_frame(static_cast<ProtocolType>(0x03), ""));
    CHECK(!encode_frame(ProtocolType::heartbeat, "x"));
    CHECK(!encode_frame(ProtocolType::message_send_request,
        std::string_view("\xED\xA0\x80", 3)));
    return 0;
}

int main()
{
    if (const int rc = test_round_trip()) { return rc; }
    if (const int rc = test_partial_bytes()) { return rc; }
    if (const int rc = test_coalesced_frames()) { return rc; }
    if (const int rc = test_protocol_errors()) { return rc; }
    if (const int rc = test_failed_decoder_stops()) { return rc; }
    if (const int rc = test_encode_rejects_invalid_input()) { return rc; }
    return 0;
}