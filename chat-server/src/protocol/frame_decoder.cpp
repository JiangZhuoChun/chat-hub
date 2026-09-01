#include "protocol/frame_decoder.h"

namespace
{

// ==================== 模块：字节序与类型校验工具 ====================
// 功能：将连续的两个大端序字节还原为无符号 16 位整数。
std::uint16_t readUint16BigEndian(const char *data)
{
    const auto high = static_cast<std::uint16_t>(static_cast<unsigned char>(data[0]));
    const auto low = static_cast<std::uint16_t>(static_cast<unsigned char>(data[1]));
    return (high << 8) | low;
}

// 功能：将连续的四个大端序字节还原为无符号 32 位整数。
std::uint32_t readUint32BigEndian(const char *data)
{
    const auto byte0 = static_cast<std::uint32_t>(static_cast<unsigned char>(data[0]));
    const auto byte1 = static_cast<std::uint32_t>(static_cast<unsigned char>(data[1]));
    const auto byte2 = static_cast<std::uint32_t>(static_cast<unsigned char>(data[2]));
    const auto byte3 = static_cast<std::uint32_t>(static_cast<unsigned char>(data[3]));
    return (byte0 << 24) | (byte1 << 16) | (byte2 << 8) | byte3;
}

} // namespace

namespace protocol
{

// ==================== 模块：增量帧解码 ====================
// 功能：缓存本次字节流，校验帧头，并将所有完整帧交给上层回调处理。
// 失败：发现魔数、版本、类型或正文长度非法时清空缓存并返回错误。
DecodeResult FrameDecoder::append(const char *data, std::size_t size, const MessageHandler &on_message)
{
    m_cache.insert(m_cache.end(), data, data + size);

    while (m_cache.size() >= kFrameHeaderLength)
    {
        const auto magic = readUint16BigEndian(m_cache.data());
        const auto version = static_cast<std::uint8_t>(static_cast<unsigned char>(m_cache[2]));
        const auto raw_type = static_cast<std::uint8_t>(static_cast<unsigned char>(m_cache[3]));
        const auto body_length = readUint32BigEndian(m_cache.data() + 4);

        if (magic != kFrameMagic)
        {
            m_cache.clear();
            return DecodeResult::invalid_magic;
        }
        if (version != kProtocolVersion)
        {
            m_cache.clear();
            return DecodeResult::unsupported_version;
        }
        if (!isKnownMessageType(raw_type))
        {
            m_cache.clear();
            return DecodeResult::unknown_message_type;
        }
        if (body_length > kMaxFrameBodyLength)
        {
            m_cache.clear();
            return DecodeResult::message_too_large;
        }

        const auto frame_length = kFrameHeaderLength + body_length;
        if (m_cache.size() < frame_length)
        {
            return DecodeResult::ok;
        }

        Message message{static_cast<MessageType>(raw_type),
                        std::string(m_cache.data() + kFrameHeaderLength, body_length)};
        m_cache.erase(m_cache.begin(), m_cache.begin() + frame_length);
        on_message(message);
    }

    return DecodeResult::ok;
}

// ==================== 模块：完整帧编码 ====================
// 功能：按照协议规定的大端序将消息类型和正文组装成完整帧。
std::string makeFrame(MessageType type, const std::string_view body)
{
    const auto body_length = static_cast<std::uint32_t>(body.size());
    std::string frame(kFrameHeaderLength, '\0');

    frame[0] = static_cast<char>((kFrameMagic >> 8) & 0xFFU);
    frame[1] = static_cast<char>(kFrameMagic & 0xFFU);
    frame[2] = kProtocolVersion;
    frame[3] = static_cast<char>(type);
    frame[4] = static_cast<char>((body_length >> 24U) & 0xFFU);
    frame[5] = static_cast<char>((body_length >> 16U) & 0xFFU);
    frame[6] = static_cast<char>((body_length >> 8U) & 0xFFU);
    frame[7] = static_cast<char>(body_length & 0xFFU);
    frame.append(body);
    return frame;
}

} // namespace protocol
