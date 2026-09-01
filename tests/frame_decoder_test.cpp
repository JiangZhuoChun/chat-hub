// M1-4 帧层单元测试：半包、粘包、空正文、超长、非法 UTF-8、未知 type、版本不兼容、往返。

#include "chathub/contracts/frame.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
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
    for (const std::uint8_t byte : bytes) {
        decoder.feed(&byte, 1);
        CHECK(!decoder.failed());
        if (decoder.next(frame)) {  // 只可能在最后一字节后产出
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
    const auto header = [](std::uint8_t hi, std::uint8_t lo, std::uint8_t version,
                            std::uint8_t type, std::uint32_t length) {
        return std::vector<std::uint8_t>{hi, lo, version, type,
            static_cast<std::uint8_t>((length >> 24) & 0xFF),
            static_cast<std::uint8_t>((length >> 16) & 0xFF),
            static_cast<std::uint8_t>((length >> 8) & 0xFF),
            static_cast<std::uint8_t>(length & 0xFF)};
    };

    {  // magic 错误：直接 failed，不重同步（D70）
        FrameDecoder decoder;
        const auto bad = header(0x00, 0x48, 1, 0x02, 0);
        decoder.feed(bad.data(), bad.size());
        CHECK(decoder.failed());
        CHECK(decoder.error() == FrameErrorKind::bad_magic);
    }
    {  // 版本不兼容（D70/D12）
        FrameDecoder decoder;
        const auto bad = header(0x43, 0x48, 2, 0x02, 0);
        decoder.feed(bad.data(), bad.size());
        CHECK(decoder.failed());
        CHECK(decoder.error() == FrameErrorKind::bad_version);
    }
    {  // 未知 type（预留 0x03）
        FrameDecoder decoder;
        const auto bad = header(0x43, 0x48, 1, 0x03, 0);
        decoder.feed(bad.data(), bad.size());
        CHECK(decoder.failed());
        CHECK(decoder.error() == FrameErrorKind::unknown_type);
    }
    {  // 4 GiB 超长：只喂帧头即拒绝，分配前生效（D09）
        FrameDecoder decoder;
        const auto bad = header(0x43, 0x48, 1, 0x50, 0xFFFFFFFF);
        decoder.feed(bad.data(), bad.size());
        CHECK(decoder.failed());
        CHECK(decoder.error() == FrameErrorKind::body_too_large);
    }
    {  // 超过该 type 上限：heartbeat max_body=0，带 1 字节正文拒绝
        FrameDecoder decoder;
        auto bad = header(0x43, 0x48, 1, 0x02, 1);
        bad.push_back('x');
        decoder.feed(bad.data(), bad.size());
        CHECK(decoder.failed());
        CHECK(decoder.error() == FrameErrorKind::body_too_large);
    }
    {  // 非法 UTF-8：0xED 0xA0 0x80 为代理区
        FrameDecoder decoder;
        const auto bad = encoded(ProtocolType::message_send_request,
            std::string("a\xED\xA0\x80z"));
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
  const auto header = [](std::uint8_t hi, std::uint8_t lo, std::uint8_t version,
                           std::uint8_t type, std::uint32_t length) {
    return std::vector<std::uint8_t>{hi, lo, version, type,
        static_cast<std::uint8_t>((length >> 24) & 0xFF),
        static_cast<std::uint8_t>((length >> 16) & 0xFF),
        static_cast<std::uint8_t>((length >> 8) & 0xFF),
        static_cast<std::uint8_t>(length & 0xFF)};
  };

    FrameDecoder decoder;
    const auto bad = header(0x43, 0x48, 9, 0x02, 0);
    decoder.feed(bad.data(), bad.size());
    CHECK(decoder.failed());
    const auto good = encoded(ProtocolType::heartbeat, "");
    decoder.feed(good.data(), good.size());  // 失败后输入被忽略
    Frame frame;
    CHECK(!decoder.next(frame));
    CHECK(decoder.failed());
    return 0;
}

int main()
{
    if (const int rc = test_round_trip()) { return rc; }
    if (const int rc = test_partial_bytes()) { return rc; }
    if (const int rc = test_coalesced_frames()) { return rc; }
    if (const int rc = test_protocol_errors()) { return rc; }
    if (const int rc = test_failed_decoder_stops()) { return rc; }
    return 0;
}