#pragma once

// 服务端单连接 TLS 握手（D62／D73）：async_handshake(server) + 5 秒超时。

#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>
#include <functional>
#include <memory>

namespace chathub::server::transport {

class TlsContext;

enum class HandshakeStatus { ok, timeout, error };

using SslStream = asio::ssl::stream<asio::ip::tcp::socket>;

// status == ok:
//   stream != nullptr，TLS 握手成功，所有权交给调用方。
//
// status == error / timeout:
//   stream == nullptr，底层连接已关闭。
//
// handler 若为空则静默结束。
using HandshakeHandler =
    std::function<void(HandshakeStatus, std::shared_ptr<SslStream>)>;

// 对一个已连接的 TCP socket 发起服务端握手；5 秒内未完成按 timeout（D73)
void asyncServerHandshake(asio::ip::tcp::socket socket, TlsContext& tls,
                          HandshakeHandler handler);
}  // namespace chathub::server::transport