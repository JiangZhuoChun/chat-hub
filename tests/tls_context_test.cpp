// M1-6 Task 1：TlsContext 加载证书链 + 私钥 + 协议策略的成功／失败路径。

#include "chathub/server/transport/tls_context.hpp"

#include <openssl/ssl.h>  // SSL_CTX_get_options / SSL_CTX_get_cipher_list

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "chathub/pki/dev_ca.hpp"

using namespace chathub;

#define CHECK(expr)                                      \
  do {                                                   \
    if (!(expr)) {                                       \
      std::fprintf(stderr, "CHECK failed: %s\n", #expr); \
      return 1;                                          \
    }                                                    \
  } while (0)

// 每次运行唯一目录，避免并行 CTest 互相删除或读取。
static std::filesystem::path uniqueTestDir() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<std::uint64_t> dist;
  return std::filesystem::temp_directory_path() /
         ("chathub-m1-6-tls-" + std::to_string(dist(gen)));
}

// 作用域清理器：无论断言早退都删除目录，不残留私钥。
struct DirCleanup {
  std::filesystem::path dir;
  ~DirCleanup() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

static bool writeTextFile(const std::filesystem::path& path,
                          const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.close();
  return static_cast<bool>(out);
}

int main() {
  const auto issued = pki::issueDevCa("127.0.0.1");
  CHECK(issued.has_value());
  const auto other = pki::issueDevCa("192.168.1.10");
  CHECK(other.has_value());

  const auto dir = uniqueTestDir();
  DirCleanup cleanup{dir};
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  CHECK(!ec);

  const auto cert = dir / "server-cert.pem";
  const auto key = dir / "server-key.pem";
  const auto other_key = dir / "other-key.pem";
  CHECK(writeTextFile(cert, issued->server_cert_pem));
  CHECK(writeTextFile(key, issued->server_key_pem));
  CHECK(writeTextFile(other_key, other->server_key_pem));

  // 成功 + 协议策略断言（TLS 1.2 下限 + ECDHE+AEAD cipher）
  {
    std::string err = "prefilled";
    auto tls = server::transport::TlsContext::load(cert, key, err);
    CHECK(tls.has_value());
    CHECK(err.empty());

    const long opts = SSL_CTX_get_options(tls->native().native_handle());
    CHECK((opts & SSL_OP_NO_TLSv1) != 0);
    CHECK((opts & SSL_OP_NO_TLSv1_1) != 0);

    // cipher 列表：TLS 1.2 只允许 ECDHE + AEAD；TLS 1.3 天然 AEAD（D126）。
    const STACK_OF(SSL_CIPHER)* ciphers =
        SSL_CTX_get_ciphers(tls->native().native_handle());
    CHECK(ciphers != nullptr);
    const int cipher_count = sk_SSL_CIPHER_num(ciphers);
    CHECK(cipher_count > 0);

    int tls12_seen = 0;
    for (int i = 0; i < cipher_count; ++i) {
      const SSL_CIPHER* cipher = sk_SSL_CIPHER_value(ciphers, i);
      const std::string s = SSL_CIPHER_get_name(cipher);
      // TLS 1.3 cipher 名以 "TLS_" 开头；ECDHE 是协议强制但不体现在名字里。
      if (s.rfind("TLS_", 0) == 0) {
        CHECK(SSL_CIPHER_is_aead(cipher) == 1);
        continue;
      }
      ++tls12_seen;
      CHECK(s.find("ECDHE") != std::string::npos);
      CHECK(SSL_CIPHER_is_aead(cipher) == 1);
    }
    CHECK(tls12_seen > 0);  // 至少配置了一个 TLS 1.2 ECDHE+AEAD 套件
  }

  // 失败：证书链缺失
  {
    std::string err;
    auto tls = server::transport::TlsContext::load(dir / "nope.pem", key, err);
    CHECK(!tls.has_value());
    CHECK(!err.empty());
  }

  // 失败：私钥缺失且 error 非空
  {
    std::string err;
    auto tls =
        server::transport::TlsContext::load(cert, dir / "no-key.pem", err);
    CHECK(!tls.has_value());
    CHECK(!err.empty());
  }

  // 失败：私钥内容不是 PEM
  CHECK(writeTextFile(dir / "bad-key.pem", "not a pem"));
  {
    std::string err;
    auto tls =
        server::transport::TlsContext::load(cert, dir / "bad-key.pem", err);
    CHECK(!tls.has_value());
    CHECK(!err.empty());
  }

  // 失败：合法但与证书不匹配的私钥（cert A + key B）
  {
    std::string err;
    auto tls = server::transport::TlsContext::load(cert, other_key, err);
    CHECK(!tls.has_value());
    CHECK(!err.empty());
  }

  return 0;
}