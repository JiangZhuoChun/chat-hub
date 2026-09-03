#include "chathub/server/transport/tls_handshake.hpp"

#include <asio.hpp>
#include <chrono>
#include <utility>

#include "chathub/server/transport/tls_context.hpp"

namespace chathub::server::transport {

void asyncServerHandshake(asio::ip::tcp::socket socket, TlsContext& tls,
                          HandshakeHandler handler) {
  using Strand = asio::strand<asio::ip::tcp::socket::executor_type>;

  struct Shared {
    Shared(Strand strand_executor, std::shared_ptr<SslStream> ssl_stream,
           HandshakeHandler callback)
        : strand(std::move(strand_executor)),
          stream(std::move(ssl_stream)),
          timer(strand),
          handler(std::move(callback)) {}

    Strand strand;
    std::shared_ptr<SslStream> stream;
    asio::steady_timer timer;
    HandshakeHandler handler;

    bool completed = false;   //最终握手流程是否已经完成并决定调用 handler
    bool timeout_started = false;   //握手结束是不是由 timer 超时导致 socket 被关闭
  };

  auto strand = asio::make_strand(socket.get_executor());
  auto stream = std::make_shared<SslStream>(std::move(socket), tls.native());
  auto state = std::make_shared<Shared>(strand, std::move(stream), std::move(handler));

  state->timer.expires_after(std::chrono::seconds(5));
  state->timer.async_wait(
      asio::bind_executor(state->strand, [state](const asio::error_code& ec) {
        if (ec || state->completed) {
          return;
        }
        // 注意：这里不能 move stream。
        // async_handshake 仍然持有并使用这个 stream。
        state->timeout_started = true;

        asio::error_code close_ec;
        state->stream->lowest_layer().close(close_ec);

        // 不在这里调用最终 handler。
        //
        // close 后正在进行的 async_handshake 会结束，
        // 最终统一由 handshake callback 收尾。
      }));

  state->stream->async_handshake(
      asio::ssl::stream_base::server,
      asio::bind_executor(state->strand, [state](const asio::error_code& ec) {
        if (state->completed) {
          return;
        }

        state->completed = true;

        asio::error_code timer_ec;
        state->timer.cancel(timer_ec);

        if (state->timeout_started) {
          // 超时路径中 socket 已经关闭。
          // 到这里 async_handshake 已经真正完成，
          // 此时才可以安全结束 Shared 生命周期。
          if (state->handler) {
            state->handler(HandshakeStatus::timeout, nullptr);
          }
          return;
        }

        if (ec) {
          // TLS 握手失败，明确关闭连接。
          asio::error_code close_ec;
          state->stream->lowest_layer().close(close_ec);
          if (state->handler) {
            state->handler(HandshakeStatus::error, nullptr);
          }
          return;
        }

        // 只有成功路径才把 stream 所有权交给调用方。
        auto stream = std::move(state->stream);

        if (state->handler) {
          state->handler(HandshakeStatus::ok, std::move(stream));
        }
      }));
}
}  // namespace chathub::server::transport