#pragma once

// 服务端共享只读 TLS 上下文（D62）：用 asio::ssl::context 加载证书链＋私钥，
// 并把最低 TLS 版本压到 1.2（D126）。加载失败即拒绝，由调用方决定是否启动服务。
// 每个连接的 ssl::stream 在 Task 3 握手时再从这里取 native() 引用。

#include <asio/ssl.hpp>

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

}  // namespace chathub::server::transport
