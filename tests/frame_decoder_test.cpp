// M1-4 帧层单元测试：半包、粘包、空正文、超长、非法 UTF-8、未知 type、版本不兼容、往返。

#include "chathub/contracts/frame.hpp"

#include <array>
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
    auto bytes = encodeFrame(type, body);
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

static int testRoundTrip()
{
    FrameDecoder decoder;
    const auto bytes = encoded(ProtocolType::message_send_request, "{\"a\":1}");
    decoder.feedBytes(bytes.data(), bytes.size());
    Frame frame;
    CHECK(decoder.tryPopFrame(frame));
    CHECK(frame.type == ProtocolType::message_send_request);
    CHECK(frame.body == "{\"a\":1}");
    CHECK(!decoder.tryPopFrame(frame));
    CHECK(!decoder.hasFailed());

    const auto heartbeat = encoded(ProtocolType::heartbeat, "");  // 空正文（D72）
    decoder.feedBytes(heartbeat.data(), heartbeat.size());
    CHECK(decoder.tryPopFrame(frame));
    CHECK(frame.type == ProtocolType::heartbeat);
    CHECK(frame.body.empty());
    return 0;
}

static int testPartialBytes()
{
    FrameDecoder decoder;
    const auto bytes = encoded(ProtocolType::login_request, "{\"account\":\"abcd\"}");
    Frame frame;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const std::uint8_t byte = bytes[i];
        decoder.feedBytes(&byte, 1);
        CHECK(!decoder.hasFailed());
        // 半包阶段不能提前交付；只有最后一个字节补齐正文后才允许产出。
        if (i + 1 < bytes.size()) {
            CHECK(!decoder.tryPopFrame(frame));
        } else {
            CHECK(decoder.tryPopFrame(frame));
            CHECK(frame.type == ProtocolType::login_request);
            CHECK(frame.body == "{\"account\":\"abcd\"}");
            Frame empty;
            CHECK(!decoder.tryPopFrame(empty));
        }
    }
    return 0;
}

static int testCoalescedFrames()
{
    FrameDecoder decoder;
    const auto first = encoded(ProtocolType::heartbeat, "");
    const auto second = encoded(ProtocolType::contact_list_request, "[]");
    std::vector<std::uint8_t> both(first);
    both.insert(both.end(), second.begin(), second.end());
    decoder.feedBytes(both.data(), both.size());
    Frame frame;
    CHECK(decoder.tryPopFrame(frame));
    CHECK(frame.type == ProtocolType::heartbeat);
    CHECK(frame.body.empty());
    CHECK(decoder.tryPopFrame(frame));
    CHECK(frame.type == ProtocolType::contact_list_request);
    CHECK(frame.body == "[]");
    CHECK(!decoder.tryPopFrame(frame));
    CHECK(!decoder.hasFailed());
    return 0;
}

static int testProtocolErrors()
{
    {  // magic 错误：直接 failed，不重同步（D70）
        FrameDecoder decoder;
        const auto bad = make_header(0x00, 0x48, 1, 0x02, 0);
        decoder.feedBytes(bad.data(), bad.size());
        CHECK(decoder.hasFailed());
        CHECK(decoder.error() == FrameErrorKind::bad_magic);
    }
    {  // 版本不兼容（D70/D12）
        FrameDecoder decoder;
        const auto bad = make_header(0x43, 0x48, 2, 0x02, 0);
        decoder.feedBytes(bad.data(), bad.size());
        CHECK(decoder.hasFailed());
        CHECK(decoder.error() == FrameErrorKind::bad_version);
    }
    {  // 未知 type（预留 0x03）
        FrameDecoder decoder;
        const auto bad = make_header(0x43, 0x48, 1, 0x03, 0);
        decoder.feedBytes(bad.data(), bad.size());
        CHECK(decoder.hasFailed());
        CHECK(decoder.error() == FrameErrorKind::unknown_type);
    }
    {  // 4 GiB 超长：只喂帧头即拒绝，分配前生效（D09）
        FrameDecoder decoder;
        const auto bad = make_header(0x43, 0x48, 1, 0x50, 0xFFFFFFFF);
        decoder.feedBytes(bad.data(), bad.size());
        CHECK(decoder.hasFailed());
        CHECK(decoder.error() == FrameErrorKind::body_too_large);
    }
    {  // 超过该 type 上限：heartbeat max_body=0，带 1 字节正文拒绝
        FrameDecoder decoder;
        auto bad = make_header(0x43, 0x48, 1, 0x02, 1);
        bad.push_back('x');
        decoder.feedBytes(bad.data(), bad.size());
        CHECK(decoder.hasFailed());
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
        decoder.feedBytes(bad.data(), bad.size());
        CHECK(decoder.hasFailed());
        CHECK(decoder.error() == FrameErrorKind::invalid_utf8);
    }
    {  // 合法 UTF-8（中文＋Emoji）不误伤
        FrameDecoder decoder;
        const auto ok = encoded(ProtocolType::message_send_request,
            "{\"text\":\"中文😀\"}");
        decoder.feedBytes(ok.data(), ok.size());
        Frame frame;
        CHECK(decoder.tryPopFrame(frame));
        CHECK(!decoder.hasFailed());
    }
    return 0;
}

static int testFailedDecoderStops()
{
    FrameDecoder decoder;
    const auto bad = make_header(0x43, 0x48, 9, 0x02, 0);
    decoder.feedBytes(bad.data(), bad.size());
    CHECK(decoder.hasFailed());
    const auto good = encoded(ProtocolType::heartbeat, "");
    decoder.feedBytes(good.data(), good.size());  // 失败后输入被忽略
    Frame frame;
    CHECK(!decoder.tryPopFrame(frame));
    CHECK(decoder.hasFailed());
    return 0;
}

static int testEncodeRejectsInvalidInput()
{
    // 出站校验与解码器的 type、长度、UTF-8 三项边界保持对称。
    CHECK(!encodeFrame(static_cast<ProtocolType>(0x03), ""));
    CHECK(!encodeFrame(ProtocolType::heartbeat, "x"));
    CHECK(!encodeFrame(ProtocolType::message_send_request,
        std::string_view("\xED\xA0\x80", 3)));
    return 0;
}

static int testUtf8SecurityBoundaries()
{
    // 覆盖 PRP §6 要求的剩余非法序列：C0/C1、超长、超出上限与截断。
    const std::array<std::string_view, 6> invalid_bodies{
        std::string_view("\xC0\x80", 2),       // C0：两字节超长编码
        std::string_view("\xC1\xBF", 2),       // C1：两字节超长编码
        std::string_view("\xE0\x80\x80", 3),   // 三字节超长编码
        std::string_view("\xF0\x80\x80\x80", 4), // 四字节超长编码
        std::string_view("\xF4\x90\x80\x80", 4), // > U+10FFFF
        std::string_view("\xC2", 1),             // 截断的两字节序列
    };

    for (const std::string_view invalid_body : invalid_bodies) {
        // 出站必须拒绝；入站测试则手工组帧，避免编码器提前拦截。
        CHECK(!encodeFrame(ProtocolType::message_send_request, invalid_body));

        FrameDecoder decoder;
        auto bytes = make_header(0x43, 0x48, 1,
            static_cast<std::uint8_t>(ProtocolType::message_send_request),
            static_cast<std::uint32_t>(invalid_body.size()));
        bytes.insert(bytes.end(), invalid_body.begin(), invalid_body.end());
        decoder.feedBytes(bytes.data(), bytes.size());
        CHECK(decoder.hasFailed());
        CHECK(decoder.error() == FrameErrorKind::invalid_utf8);
    }
    return 0;
}

int main()
{
    if (const int rc = testRoundTrip()) { return rc; }
    if (const int rc = testPartialBytes()) { return rc; }
    if (const int rc = testCoalescedFrames()) { return rc; }
    if (const int rc = testProtocolErrors()) { return rc; }
    if (const int rc = testFailedDecoderStops()) { return rc; }
    if (const int rc = testEncodeRejectsInvalidInput()) { return rc; }
    if (const int rc = testUtf8SecurityBoundaries()) { return rc; }
    return 0;
}