// M1-5 开发 CA 单元测试：IP 校验、证书合同、链签名、文件落盘。

#include "chathub/pki/dev_ca.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace chathub::pki;

#define CHECK(expr)                                          \
    do {                                                     \
        if (!(expr)) {                                       \
            std::fprintf(stderr, "CHECK failed: %s\n", #expr); \
            return 1;                                        \
        }                                                    \
    } while (0)

// P-256：EC 公钥且 256 bit。
static bool is_p256(const CertFacts& f) { return f.is_ec && f.ec_bits == 256; }

static int test_ipv4_literal() {
    CHECK(isIpv4Literal("127.0.0.1"));
    CHECK(isIpv4Literal("0.0.0.0"));
    CHECK(isIpv4Literal("255.255.255.255"));
    CHECK(!isIpv4Literal(""));
    CHECK(!isIpv4Literal("127.0.0"));
    CHECK(!isIpv4Literal("127.0.0.1.2"));
    CHECK(!isIpv4Literal("127.0.0.256"));
    CHECK(!isIpv4Literal("01.2.3.4"));     // 前导零
    CHECK(!isIpv4Literal("127.0.0.01"));
    CHECK(!isIpv4Literal("127.0.0.a"));
    CHECK(!isIpv4Literal("localhost"));
    CHECK(!isIpv4Literal("::1"));          // IPv6
    CHECK(!isIpv4Literal("127.0.0.1 "));   // 尾随空白
    return 0;
}

static int test_issue_contract() {
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
    CHECK(is_p256(*ca));
    CHECK(ca->signature_oid == "1.2.840.10045.4.3.2");  // ecdsa-with-SHA256
    const auto ca_days = (ca->not_after - ca->not_before) / 86400;
    CHECK(ca_days >= 3640 && ca_days <= 3660);           // 约 10 年

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
    CHECK(is_p256(*server));
    CHECK(server->signature_oid == "1.2.840.10045.4.3.2");
    const auto leaf_days = (server->not_after - server->not_before) / 86400;
    CHECK(leaf_days >= 360 && leaf_days <= 370);         // 约 1 年

    // ---- 链签名 ----
    CHECK(verifySignedBy(issued->ca_cert_pem, issued->ca_cert_pem));       // 根自签
    CHECK(verifySignedBy(issued->server_cert_pem, issued->ca_cert_pem));   // 叶由根签
    CHECK(!verifySignedBy(issued->ca_cert_pem, issued->server_cert_pem));  // 反向否决

    // ---- 两次签发指纹不同（序列号独立随机）----
    const auto other = issueDevCa("192.168.1.10");
    CHECK(other.has_value());
    CHECK(other->ca_fingerprint_sha256 != issued->ca_fingerprint_sha256);
    const auto other_server = inspectCertPem(other->server_cert_pem);
    CHECK(other_server.has_value());
    CHECK(other_server->san_ips.front() == "192.168.1.10");
    return 0;
}

static int test_write_files() {
    const auto issued = issueDevCa("10.0.0.1");
    CHECK(issued.has_value());

    const auto dir = std::filesystem::temp_directory_path() / "chathub-m1-5-pki";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    CHECK(writeDevCaFiles(*issued, dir.string()));

    std::ifstream in(dir / "trusted-ca.pem", std::ios::binary);
    CHECK(in.good());
    const std::string pem((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    const auto facts = inspectCertPem(pem);
    CHECK(facts.has_value() && facts->is_ca);  // 信任包只应含根公钥

    std::filesystem::remove_all(dir, ec);
    return 0;
}

int main() {
    if (const int rc = test_ipv4_literal()) return rc;
    if (const int rc = test_issue_contract()) return rc;
    if (const int rc = test_write_files()) return rc;
    return 0;
}