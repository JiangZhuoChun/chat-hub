// M1-5 开发 CA 单元测试：IP 校验、证书合同、链签名、私钥配对、文件落盘。

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "chathub/pki/dev_ca.hpp"

using namespace chathub::pki;

#define CHECK(expr)                                      \
  do {                                                   \
    if (!(expr)) {                                       \
      std::fprintf(stderr, "CHECK failed: %s\n", #expr); \
      return 1;                                          \
    }                                                    \
  } while (0)

// P-256：EC 公钥、256 bit，且曲线名为 prime256v1（或 P-256）。
static bool isP256(const CertFacts& f) {
  return f.is_ec && f.ec_bits == 256 &&
         (f.group_name == "prime256v1" || f.group_name == "P-256");
}

// 作用域清理器：无论断言早退都删除测试目录。
struct DirCleanup {
  std::filesystem::path dir;
  ~DirCleanup() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

static int testIpv4Literal() {
  CHECK(isIpv4Literal("127.0.0.1"));
  CHECK(isIpv4Literal("0.0.0.0"));
  CHECK(isIpv4Literal("255.255.255.255"));
  CHECK(!isIpv4Literal(""));
  CHECK(!isIpv4Literal("127.0.0"));
  CHECK(!isIpv4Literal("127.0.0.1.2"));
  CHECK(!isIpv4Literal("127.0.0.256"));
  CHECK(!isIpv4Literal("01.2.3.4"));  // 前导零
  CHECK(!isIpv4Literal("127.0.0.01"));
  CHECK(!isIpv4Literal("127.0.0.a"));
  CHECK(!isIpv4Literal("localhost"));
  CHECK(!isIpv4Literal("::1"));         // IPv6
  CHECK(!isIpv4Literal("127.0.0.1 "));  // 尾随空白
  return 0;
}

static int testIssueContract() {
  CHECK(!issueDevCa("localhost"));
  CHECK(!issueDevCa("256.1.1.1"));

  const auto issued = issueDevCa(kDefaultServerIpv4);
  CHECK(issued.has_value());
  CHECK(issued->ca_fingerprint_sha256.size() == 64);

  const auto ca = inspectCertPem(issued->ca_cert_pem);
  const auto server = inspectCertPem(issued->server_cert_pem);
  CHECK(ca.has_value());
  CHECK(server.has_value());

  // ---- 根证书（D126）----
  CHECK(ca->is_ca);
  CHECK(ca->key_cert_sign);
  CHECK(ca->crl_sign);
  CHECK(!ca->digital_signature);
  CHECK(!ca->server_auth);
  CHECK(ca->san_ips.empty());
  CHECK(!ca->has_dns_san && !ca->has_ipv6_san);
  CHECK(ca->serial_bit_length == 128);
  CHECK(isP256(*ca));
  CHECK(ca->signature_oid == "1.2.840.10045.4.3.2");  // ecdsa-with-SHA256
  const auto ca_days = (ca->not_after - ca->not_before) / 86400;
  CHECK(ca_days >= 3640 && ca_days <= 3660);  // 约 10 年

  // ---- 叶证书（D126）----
  CHECK(!server->is_ca);
  CHECK(!server->key_cert_sign);
  CHECK(!server->crl_sign);
  CHECK(server->digital_signature);
  CHECK(server->server_auth);
  CHECK(!server->has_dns_san && !server->has_ipv6_san);
  CHECK(server->san_ips.size() == 1);
  CHECK(server->san_ips.front() == "127.0.0.1");
  CHECK(server->serial_bit_length == 128);
  CHECK(isP256(*server));
  CHECK(server->signature_oid == "1.2.840.10045.4.3.2");
  const auto leaf_days = (server->not_after - server->not_before) / 86400;
  CHECK(leaf_days >= 360 && leaf_days <= 370);  // 约 1 年

  // ---- 链签名 ----
  CHECK(verifySignedBy(issued->ca_cert_pem, issued->ca_cert_pem));  // 根自签
  CHECK(verifySignedBy(issued->server_cert_pem,
                       issued->ca_cert_pem));  // 叶由根签
  CHECK(!verifySignedBy(issued->ca_cert_pem,
                        issued->server_cert_pem));  // 反向否决

  // ---- 证书与私钥匹配，且根/叶私钥独立 ----
  CHECK(certMatchesKey(issued->ca_cert_pem, issued->ca_key_pem));
  CHECK(certMatchesKey(issued->server_cert_pem, issued->server_key_pem));
  CHECK(!certMatchesKey(issued->ca_cert_pem,
                        issued->server_key_pem));  // 根证书 ≠ 叶私钥
  CHECK(!certMatchesKey(issued->server_cert_pem,
                        issued->ca_key_pem));  // 叶证书 ≠ 根私钥

  // ---- 同批和跨两次签发的序列号相互独立 ----
  CHECK(ca->serial_hex != server->serial_hex);
  const auto other = issueDevCa("192.168.1.10");
  CHECK(other.has_value());
  const auto other_ca = inspectCertPem(other->ca_cert_pem);
  CHECK(other_ca.has_value());
  CHECK(other_ca->serial_hex != ca->serial_hex);
  const auto other_server = inspectCertPem(other->server_cert_pem);
  CHECK(other_server.has_value());
  CHECK(other_server->san_ips.size() == 1);
  CHECK(other_server->san_ips.front() == "192.168.1.10");
  return 0;
}

static int testWriteFiles() {
  const auto issued = issueDevCa("10.0.0.1");
  CHECK(issued.has_value());

  const auto dir = std::filesystem::temp_directory_path() /
                   ("chathub-m1-5-pki-" +
                    issued->ca_fingerprint_sha256);
  DirCleanup cleanup{dir};
  CHECK(writeDevCaFiles(*issued, dir.string()));

  // 四个文件都存在，且内容与签发结果一致。
  const auto readAll = [](const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  };
  CHECK(readAll(dir / "trusted-ca.pem") == issued->ca_cert_pem);
  CHECK(readAll(dir / "ca-key.pem") == issued->ca_key_pem);
  CHECK(readAll(dir / "server-cert.pem") == issued->server_cert_pem);
  CHECK(readAll(dir / "server-key.pem") == issued->server_key_pem);

  // 信任包只应含根公钥。
  const auto facts = inspectCertPem(issued->ca_cert_pem);
  CHECK(facts.has_value() && facts->is_ca);
  return 0;
}

int main() {
  if (const int rc = testIpv4Literal()) return rc;
  if (const int rc = testIssueContract()) return rc;
  if (const int rc = testWriteFiles()) return rc;
  return 0;
}
