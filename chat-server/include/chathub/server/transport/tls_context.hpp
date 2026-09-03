#pragma once

// 服务端共享只读 TLS 上下文（D62）：用 asio::ssl::context 加载证书链＋私钥，
// 并把最低 TLS 版本压到 1.2（D126）。加载失败即拒绝，由调用方决定是否启动服务。
// 每个连接的 ssl::stream 在 Task 3 握手时再从这里取 native() 引用。

#include <asio/ssl.hpp>
#include <openssl/ssl.h>  // SSL_CTX_set_cipher_list / SSL_CTX_check_private_key

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace chathub::server::transport{

class TlsContext {
public:
  // 加载证书链(PEM)＋私钥(PEM) 并校验两者匹配。失败返回 nullopt 并写 error。
  // 注意：非 noexcept —— 极端情况（如分配失败）由异常传播，文件/OpenSSL 错误走 error 返回。
  // 证书路径约定为部署可控的 ASCII 路径；当前窄字符串接口不支持中文路径。
  [[nodiscard]] static std::optional<TlsContext> load(const std::filesystem::path& cert_chain,
    const std::filesystem::path& private_key,std::string& error);

  TlsContext(TlsContext&&) noexcept = default;
  TlsContext& operator=(TlsContext&&) noexcept = default;
  //TLS context 是一种拥有底层 OpenSSL 资源的对象,禁止复制
  TlsContext(const TlsContext&) noexcept = delete;
  TlsContext& operator=(const TlsContext&) noexcept = delete;

  //把封装在 TlsContext 内部的真实 asio::ssl::context 暴露给需要创建 TLS stream 的地方
  // moved-from 对象只能析构，不再调用 native()。
  asio::ssl::context& native() noexcept { return *context_; }
  [[nodiscard]] const asio::ssl::context& native() const noexcept { return *context_; }

private:
  TlsContext() = default;
  // 用 unique_ptr 持有：不依赖 asio 版本对 ssl::context 的移动支持。
  std::unique_ptr<asio::ssl::context> context_;
};

inline std::optional<TlsContext> TlsContext::load(
    const std::filesystem::path& cert_chain,
    const std::filesystem::path& private_key, std::string& error){

  namespace ssl = asio::ssl;
  std::error_code ec;

  error.clear();  // 成功后置条件：error 为空

  //明确告诉 Asio：这个 TLS context 用于服务端
  auto ctx = std::make_unique<ssl::context>(ssl::context::tls_server);

  // 仅允许 ≥TLS1.2（D126）。no_tlsv1/no_tlsv1_1 在 OpenSSL 3.5 下若编译失败可删。
  ctx->set_options(ssl::context::default_workarounds |
                      ssl::context::no_sslv2 |
                      ssl::context::no_sslv3 |
                      ssl::context::no_tlsv1 |
                      ssl::context::no_tlsv1_1);
  // TLS 1.2 只允许 ECDHE＋AEAD（D126）：服务器证书固定 ECDSA P-256。
  // TLS 1.3 使用默认 AEAD 套件，不受此列表影响；1.3 优先协商（D126）。
  if (SSL_CTX_set_cipher_list(ctx->native_handle(),
    "ECDHE+ECDSA+AESGCM:ECDHE+ECDSA+CHACHA20") != 1) {
    error = "configure TLS 1.2 cipher list (ECDHE+AEAD) failed";
    return std::nullopt;
  }

  //加载证书链
  ctx->use_certificate_chain_file(cert_chain.string(),ec);
  if (ec) {
    error = "load cert chain '" + cert_chain.string() + "': " + ec.message();
    return std::nullopt;
  }
  //加载私钥
  ctx->use_private_key_file(private_key.string(),ssl::context::pem,ec);
  if (ec) {
    error = "load private key '" + private_key.string() + "': " + ec.message();
    return std::nullopt;
  }

  // 显式校验证书公钥与私钥匹配；不匹配在启动时拒绝，而非拖到握手。
  if (SSL_CTX_check_private_key(ctx->native_handle()) != 1) {
    error = "private key does not match certificate";
    return std::nullopt;
  }

  TlsContext out;
  out.context_ = std::move(ctx);
  return out;
}

}  // namespace chathub::server::transport