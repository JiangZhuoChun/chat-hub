#pragma once

// 开发用私有 CA（D06、D125—D127）：
// 仅用 OpenSSL 生成 ECDSA P-256 + SHA-256 的自签根与服务器叶证书；
// 根 10 年、叶 1 年；SAN 仅 IPv4；根私钥与证书密钥不进入 Git。
// 本头不暴露任何 OpenSSL 类型，测试只依赖纯 C++ 字段。

#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chathub::pki{

inline constexpr std::string_view kDefaultServerIpv4 = "127.0.0.1";
inline constexpr int kCaValidityDays = 3650;
inline constexpr int kServerValidityDays = 365;

// 严格点分 IPv4：四段 0—255，无前导零、无尾随空白、拒绝主机名/IPv6。
bool isIpv4Literal(std::string_view text) noexcept;

// 签发结果：PEM 文本与根证书指纹（DER 的 SHA-256，64 位小写 hex）。
struct IssueDevCa {
  std::string ca_cert_pem;
  std::string ca_key_pem;
  std::string server_cert_pem;
  std::string server_key_pem;
  std::string ca_fingerprint_sha256;
};

std::optional<IssueDevCa> issueDevCa(std::string_view ipv4);

// trusted-ca.pem（仅根公钥）＋ ca-key.pem / server-cert.pem / server-key.pem。
bool writeDevCaFiles(const IssueDevCa& issued,std::string_view dir);

// 证书合同事实：供测试断言 D125/D126，不暴露 OpenSSL 类型。
struct CertFacts {
  bool is_ca = false;
  bool key_cert_sign = false;
  bool crl_sign = false;
  bool digital_signature = false;
  bool server_auth = false;
  bool has_dns_san = false;
  bool has_ipv6_san = false;
  bool is_ec = false;
  int ec_bits = 0;              // 256 即 P-256
  int serial_bit_length = 0;
  std::string signature_oid;    // 如 "1.2.840.10045.4.3.2"（ecdsa-with-SHA256）
  std::vector<std::string> san_ips;  // 仅 IPv4
  std::time_t not_before = 0;
  std::time_t not_after = 0;
};

std::optional<CertFacts> inspectCertPem(std::string_view pem);

// cert 是否由 issuer 的私钥签发（用 X509_verify，不做完整链/时间校验）。
bool verifySignedBy(std::string_view cert_pem,std::string_view issuer_pem);

}