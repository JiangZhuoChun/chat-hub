#include "chathub/server/transport/tls_context.hpp"

#include <openssl/ssl.h>

#include <system_error>
#include <utility>

namespace chathub::server::transport {

std::optional<TlsContext> TlsContext::load(
    const std::filesystem::path& cert_chain,
    const std::filesystem::path& private_key,
    std::string& error) {
  namespace ssl = asio::ssl;

  error.clear();

  auto context = std::make_unique<ssl::context>(ssl::context::tls_server);
  context->set_options(ssl::context::default_workarounds |
                       ssl::context::no_sslv2 |
                       ssl::context::no_sslv3 |
                       ssl::context::no_tlsv1 |
                       ssl::context::no_tlsv1_1);

  // TLS 1.2 只允许 ECDHE＋AEAD；TLS 1.3 不受此列表影响。
  if (SSL_CTX_set_cipher_list(
          context->native_handle(),
          "ECDHE+ECDSA+AESGCM:ECDHE+ECDSA+CHACHA20") != 1) {
    error = "configure TLS 1.2 cipher list (ECDHE+AEAD) failed";
    return std::nullopt;
  }

  std::error_code ec;
  context->use_certificate_chain_file(cert_chain.string(), ec);
  if (ec) {
    error = "load cert chain '" + cert_chain.string() + "': " + ec.message();
    return std::nullopt;
  }

  context->use_private_key_file(private_key.string(), ssl::context::pem, ec);
  if (ec) {
    error = "load private key '" + private_key.string() + "': " + ec.message();
    return std::nullopt;
  }

  if (SSL_CTX_check_private_key(context->native_handle()) != 1) {
    error = "private key does not match certificate";
    return std::nullopt;
  }

  TlsContext result;
  result.context_ = std::move(context);
  return result;
}

}  // namespace chathub::server::transport
