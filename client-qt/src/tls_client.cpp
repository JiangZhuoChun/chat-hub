#include "chathub/client/infrastructure/tls_client.hpp"

namespace chathub::client::infrastructure {

bool TlsClientConfig::isBackendReady(QString& error)
{
  if (!QSslSocket::supportsSsl()) {
    error = QStringLiteral("no TLS backend available");
    return false;
  }
  // sslLibraryVersionString 形如 "OpenSSL 3.5.4 30 Sep 2025"：含 "3.5" 即 3.5 系列。
  const QString version = QSslSocket::sslLibraryVersionString();
  if (!version.contains(QLatin1String("3.5"))) {
    error = QStringLiteral("unexpected OpenSSL version: ") + version;
    return false;
  }
  return true;
}

std::optional<TlsClientConfig> TlsClientConfig::build(
    const QString& ca_file_path, QString& error)
{
  error.clear();

  if (!isBackendReady(error)) {
    return std::nullopt;
  }

  // 只加载项目私有 CA（D62）；空列表视为失败。
  const QList<QSslCertificate> certs = QSslCertificate::fromPath(ca_file_path);
  if (certs.isEmpty()) {
    error = QStringLiteral("no CA certificate loaded from: ") + ca_file_path;
    return std::nullopt;
  }

  TlsClientConfig out;
  out.config_.setCaCertificates(certs);
  // VerifyPeer：证书链或 IP 不匹配即失败，绝不 ignoreSslErrors（D62）。
  out.config_.setPeerVerifyMode(QSslSocket::VerifyPeer);
  return out;
}

void TlsClientConfig::applyTo(QSslSocket& socket) const
{
  socket.setSslConfiguration(config_);
  socket.setPeerVerifyMode(QSslSocket::VerifyPeer);
}

}  // namespace chathub::client::infrastructure