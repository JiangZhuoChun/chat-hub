#pragma once

// 客户端 TLS 配置（D62／D63）：只信任项目私有 CA，VerifyPeer，校验服务器 IP。
// 本头只出现在 infrastructure（网络 adapter）层；domain／application 不得包含。

#include <QList>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QString>

#include <optional>

namespace chathub::client::infrastructure {

class TlsClientConfig {
public:
  // 检查 OpenSSL 后端可用且为 3.5 LTS（D63）。失败写 error 并返回 false。
  [[nodiscard]] static bool isBackendReady(QString& error);

  // 从 CA 文件构建只含项目私有 CA 的配置 + VerifyPeer。
  [[nodiscard]] static std::optional<TlsClientConfig> build(
      const QString& ca_file_path, QString& error);

  TlsClientConfig(const TlsClientConfig&) = default;
  TlsClientConfig& operator=(const TlsClientConfig&) = default;

  // 应用到 socket；Task 3 再 connectToHostEncrypted(ip, port, ip) 并等 encrypted()。
  void applyTo(QSslSocket& socket) const;

  [[nodiscard]] const QSslConfiguration& configuration() const noexcept {
    return config_;
  }

private:
  TlsClientConfig() = default;
  QSslConfiguration config_;
};

}  // namespace chathub::client::infrastructure