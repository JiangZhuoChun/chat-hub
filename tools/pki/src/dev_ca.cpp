#include "chathub/pki/dev_ca.hpp"

// ┌─────────────────────────────────────────────────────┐
// │  文本 I/O    <openssl/pem.h>     PEM_write_* / PEM_read_* │
// │  证书验证    <openssl/x509_vfy.h> X509_STORE / X509_verify_cert │
// │  v3 扩展     <openssl/x509v3.h>   X509V3_CTX / EXT_conf_nid / check_ip │
// ├─────────────────────────────────────────────────────┤
// │  证书核心    <openssl/x509.h>     X509 / X509_NAME / X509_sign │
// │  加密原语    <openssl/evp.h>      EVP_PKEY / EVP_MD / EVP_EC_gen │
// ├─────────────────────────────────────────────────────┤
// │  OID 注册表  <openssl/objects.h>  NID_* 常量 / OBJ_* 查询 │
// │  底层基础    <openssl/bn.h>       BIGNUM 大数         │
// │  底层基础    <openssl/asn1.h>     ASN1_INTEGER / TIME / STRING │
// └─────────────────────────────────────────────────────┘
#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace chathub::pki {
namespace {
// RAII：OpenSSL C 指针随 unique_ptr 自动释放，异常/早退也不泄漏。
struct PkeyDel {
  void operator()(EVP_PKEY* p) const noexcept { EVP_PKEY_free(p); }
};
struct X509Del {
  void operator()(X509* p) const noexcept { X509_free(p); }
};
struct BioDel {
  void operator()(BIO* p) const noexcept { BIO_free(p); }
};
struct BnDel {
  void operator()(BIGNUM* p) const noexcept { BN_free(p); }
};

using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDel>;
using X509Ptr = std::unique_ptr<X509, X509Del>;
using BioPtr = std::unique_ptr<BIO, BioDel>;
using BnPtr = std::unique_ptr<BIGNUM, BnDel>;

// 给 X.509 证书添加一个扩展字段；issuer/subject 供 SAN、AKI
// 等需要上下文的扩展使用。
// cert     最终要往哪张证书里添加扩展   issuer   这张证书的签发者
// subject  这张证书本身        nid      扩展类型
// value    扩展内容
bool addExt(X509* cert, X509* issuer, X509* subject, int nid,
            const char* value) {
  // 1.创建一个X.509 v3 扩展构造上下文。
  X509V3_CTX ctx;
  // 2.初始化 ctx
  X509V3_set_ctx(&ctx, issuer, subject, nullptr, nullptr, 0);
  // 3.根据 nid + value 创建一个 X509_EXTENSION 对象
  X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
  if (!ext) return false;

  const int ok = X509_add_ext(cert, ext, -1);  // -1：追加到末尾
  X509_EXTENSION_free(ext);

  return ok == 1;
}

// 负责设置：X.509 Serial Number,也就是证书序列号。
bool setSerial(X509* cert) {
  const BnPtr bn(BN_new());
  if (!bn) return false;
  // 强制最高 bit 为 1,不对最低位做特殊要求
  if (BN_rand(bn.get(), 128, BN_RAND_TOP_ONE, BN_RAND_BOTTOM_ANY) != 1) {
    return false;
  }
  ASN1_INTEGER* serial = BN_to_ASN1_INTEGER(bn.get(), nullptr);
  if (!serial) return false;

  const int ok = X509_set_serialNumber(cert, serial);
  ASN1_INTEGER_free(serial);

  return ok == 1;
}

// 根与叶共用的公共初始化：v3、序列号、有效期、CN、公钥。
bool initCert(X509* cert, EVP_PKEY* key, const char* cn, int days) {
  if (X509_set_version(cert, X509_VERSION_3) != 1) return false;

  if (!setSerial(cert)) return false;

  if (X509_gmtime_adj(X509_getm_notBefore(cert), 0) == nullptr) return false;

  if (X509_gmtime_adj(X509_getm_notAfter(cert),
                      static_cast<long>(days) * 86400L) == nullptr) {
    return false;
  }

  X509_NAME* name = X509_get_subject_name(cert);
  if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                 reinterpret_cast<const unsigned char*>(cn), -1,
                                 -1, 0) != 1) {
    return false;
  }
  return X509_set_pubkey(cert, key);
}

/*----------------------
OpenSSL 对象 → PEM 文本
PEM 文本 → OpenSSL 对象
------------------------*/
std::string bioToString(BIO* bio) {
  char* date = nullptr;
  // data -> BIO内部缓冲区;n -> 这块缓冲区当前有多少字节
  const long n = BIO_get_mem_data(bio, &date);

  return (date && n > 0) ? std::string(date, static_cast<std::size_t>(n))
                         : std::string{};
}
std::string x509ToPem(X509* cert) {
  // 有一块内存，请让 BIO 从这里读取
  const BioPtr bio(BIO_new(BIO_s_mem()));
  // 把 X509 对象编码成 PEM 格式，然后写进 BIO
  if (!bio || PEM_write_bio_X509(bio.get(), cert) != 1) return {};

  return bioToString(bio.get());
}
std::string pkeyToPem(EVP_PKEY* key) {
  const BioPtr bio(BIO_new(BIO_s_mem()));
  // 把私钥对象转换成 PEM 私钥文本
  // bio    写到哪里    key    要写哪个私钥  cipher    用什么算法加密私钥
  // kstr   密码缓冲区  klen   密码长度      callback  密码回调     userdata
  // 回调用户数据
  if (!bio || PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0,
                                       nullptr, nullptr) != 1) {
    return {};
  }
  return bioToString(bio.get());
}
// BIO_new_mem_buf 接受 int 长度，窄化前先拒绝异常大的输入。
BioPtr memBio(std::string_view data) {
  if (data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return {};
  }
  return BioPtr(BIO_new_mem_buf(data.data(), static_cast<int>(data.size())));
}
X509Ptr pemToX509(std::string_view pem) {
  // 创建一个“读取型内存 BIO”
  const BioPtr bio = memBio(pem);
  if (!bio) return {};

  return X509Ptr(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
}

// 计算一张 X.509 证书的 SHA-256 指纹，并把结果转成十六进制字符串。
std::string fingerPrint(X509* cert) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int n = 0;
  // 对整个 X.509 证书做 SHA-256 摘要
  if (X509_digest(cert, EVP_sha256(), md, &n) != 1) return {};

  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);

  for (auto i = 0; i < n; ++i) {
    out.push_back(hex[md[i] >> 4]);    // 取高 4 位
    out.push_back(hex[md[i] & 0x0f]);  // 取低 4 位
  }
  return out;
}

std::time_t asn1ToTime(const ASN1_TIME* when) {
  std::tm t = {};
  if (!when || ASN1_TIME_to_tm(when, &t) != 1) return 0;
#ifdef _WIN32
  return _mkgmtime(&t);
#else
  return timegm(&t);
#endif
}

}  // namespace

// 判断一个字符串是否是有效的 IPv4 地址字面量。
bool isIpv4Literal(std::string_view text) noexcept {
  int octets = 0;
  std::string_view rest = text;

  while (true) {
    const std::size_t dot = rest.find('.');
    const std::string_view octet = rest.substr(0, dot);

    if (octet.empty() || octet.size() > 3) return false;
    if (octet.size() > 1 && octet[0] == '0') return false;

    int value = 0;
    for (const char c : octet) {
      if (c < '0' || c > '9') return false;
      // 计算十进制值
      value = value * 10 + (c - '0');
    }
    if (value > 255) return false;
    ++octets;
    if (dot == std::string_view::npos) break;
    // 跳过点号
    rest.remove_prefix(dot + 1);
  }
  return octets == 4;
}
// 创建一个开发用的根证书和一个服务器证书
std::optional<IssueDevCa> issueDevCa(std::string_view ipv4) {
  if (!isIpv4Literal(ipv4)) return std::nullopt;

  const std::string ip(ipv4);
  const std::string san = "IP:" + ip;

  PkeyPtr ca_key(EVP_EC_gen("P-256"));
  PkeyPtr leaf_key(EVP_EC_gen("P-256"));
  X509Ptr ca(X509_new());
  X509Ptr leaf(X509_new());

  if (!ca_key || !leaf_key || !ca || !leaf) return std::nullopt;

  // 根证书：自签，CA:TRUE（D126）
  if (!initCert(ca.get(), ca_key.get(), "ChatHub Dev Root CA",
                kCaValidityDays)) {
    return std::nullopt;
  }
  // 设置根证书的发行者名称
  if (X509_set_issuer_name(ca.get(), X509_get_subject_name(ca.get())) != 1) {
    return std::nullopt;
  }
  if (!addExt(ca.get(), ca.get(), ca.get(), NID_basic_constraints,
              "critical,CA:TRUE") ||
      !addExt(ca.get(), ca.get(), ca.get(), NID_key_usage,
              "critical,keyCertSign,cRLSign") ||
      !addExt(ca.get(), ca.get(), ca.get(), NID_subject_key_identifier,
              "hash")) {
    return std::nullopt;
  }
  if (X509_sign(ca.get(), ca_key.get(), EVP_sha256()) == 0) return std::nullopt;
  // 叶证书：根签发，CA:FALSE，SAN 仅给定 IPv4（D126）。
  if (!initCert(leaf.get(), leaf_key.get(), ip.c_str(), kServerValidityDays)) {
    return std::nullopt;
  }
  if (X509_set_issuer_name(leaf.get(), X509_get_subject_name(ca.get())) != 1) {
    return std::nullopt;
  }
  if (!addExt(leaf.get(), ca.get(), leaf.get(), NID_basic_constraints,
              "critical,CA:FALSE") ||
      !addExt(leaf.get(), ca.get(), leaf.get(), NID_key_usage,
              "critical,digitalSignature") ||
      !addExt(leaf.get(), ca.get(), leaf.get(), NID_ext_key_usage,
              "serverAuth") ||
      !addExt(leaf.get(), ca.get(), leaf.get(), NID_subject_alt_name,
              san.c_str()) ||
      !addExt(leaf.get(), ca.get(), leaf.get(), NID_authority_key_identifier,
              "keyid:always")) {
    return std::nullopt;
  }
  if (X509_sign(leaf.get(), ca_key.get(), EVP_sha256()) == 0)
    return std::nullopt;

  IssueDevCa out;
  out.ca_cert_pem = x509ToPem(ca.get());
  out.ca_key_pem = pkeyToPem(ca_key.get());
  out.server_cert_pem = x509ToPem(leaf.get());
  out.server_key_pem = pkeyToPem(leaf_key.get());
  out.ca_fingerprint_sha256 = fingerPrint(ca.get());

  if (out.ca_cert_pem.empty() || out.ca_key_pem.empty() ||
      out.server_cert_pem.empty() || out.server_key_pem.empty() ||
      out.ca_fingerprint_sha256.empty()) {
    return std::nullopt;
  }
  return out;
}

// 将开发用的根证书和服务器证书写入合同约定的四个文件。
bool writeDevCaFiles(const IssueDevCa& issued, std::string_view output_dir) {
  std::error_code ec;
  const std::filesystem::path output_path{std::string(output_dir)};
  std::filesystem::create_directories(output_path, ec);

  if (ec) return false;

  const auto write_file = [&](const char* name, std::string_view text) {
    std::ofstream out(output_path / name, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();  // 让最终 flush/close 错误反映到流状态。
    return static_cast<bool>(out);
  };

  return write_file("trusted-ca.pem", issued.ca_cert_pem) &&
         write_file("ca-key.pem", issued.ca_key_pem) &&
         write_file("server-cert.pem", issued.server_cert_pem) &&
         write_file("server-key.pem", issued.server_key_pem);
}

//把 PEM 证书解析成 X509，然后只提取测试真正关心的事实，装进 CertFacts。
std::optional<CertFacts> inspectCertPem(std::string_view pem) {
  X509Ptr cert = pemToX509(pem);
  if (!cert) return std::nullopt;

  CertFacts f;

  // CA 标志（basicConstraints）
  f.is_ca = X509_check_ca(cert.get()) == 1;

  // KeyUsage 位：0=digitalSignature，5=keyCertSign，6=cRLSign（RFC 5280 位序）。
  if (auto* ku = static_cast<ASN1_BIT_STRING*>
    //找到指定扩展，并且直接把扩展里的 DER/ASN.1 内容解码成 OpenSSL 对象
    (X509_get_ext_d2i(cert.get(), NID_key_usage, nullptr, nullptr))) {

    //根 CA 应该有：keyCertSign = true   cRLSign = true
    //服务器叶： digitalSignature = true
    f.digital_signature = ASN1_BIT_STRING_get_bit(ku, 0) == 1;
    f.key_cert_sign = ASN1_BIT_STRING_get_bit(ku, 5) == 1;
    f.crl_sign = ASN1_BIT_STRING_get_bit(ku, 6) == 1;

    ASN1_BIT_STRING_free(ku);
  }

  // ExtKeyUsage：查找 serverAuth（OID 1.3.6.1.5.5.7.3.1）
  //本质上：ASN1_OBJECT 的数组
  if (auto* eku = static_cast<EXTENDED_KEY_USAGE*>
    (X509_get_ext_d2i(cert.get(), NID_ext_key_usage, nullptr, nullptr))) {

    for (int i = 0; i < sk_ASN1_OBJECT_num(eku);++i) {
      //把 OID 转成 OpenSSL 内部 NID
      if (OBJ_obj2nid(sk_ASN1_OBJECT_value(eku,i)) == NID_server_auth) {
        f.server_auth = true;
      }
    }
    EXTENDED_KEY_USAGE_free(eku);
  }

  // SAN：区分 DNS / IPv4(4 字节) / IPv6(16 字节)。
  if (auto* names = static_cast<GENERAL_NAMES*>
    (X509_get_ext_d2i(cert.get(), NID_subject_alt_name, nullptr, nullptr))) {

    for (int i = 0; i < sk_GENERAL_NAME_num(names); ++i) {
      const GENERAL_NAME* g = sk_GENERAL_NAME_value(names, i);

      if (g->type == GEN_DNS) {
        f.has_dns_san = true;
      }else if (g->type == GEN_IPADD) {
        const auto* ip = g->d.iPAddress;

        if (ip->length == 4) {
          f.san_ips.push_back(std::to_string(ip->data[0]) + "." +
                              std::to_string(ip->data[1]) + "." +
                              std::to_string(ip->data[2]) + "." +
                              std::to_string(ip->data[3]));
        }else if (ip->length == 16){
          f.has_ipv6_san = true;
        }
      }
    }
    GENERAL_NAMES_free(names);
  }

  // 公钥：仅 EC 且 256 bit 判为 P-256。
  if (auto* pub = X509_get_pubkey(cert.get())) {
    //判断是不是：EC / 椭圆曲线公钥
    f.is_ec = EVP_PKEY_base_id(pub) == EVP_PKEY_EC;
    f.ec_bits = EVP_PKEY_bits(pub);
    // 曲线名用于区分 P-256 与其他 256 位曲线。
    if (f.is_ec) {
      char gname[64] = {0};
      if (EVP_PKEY_get_group_name(pub,gname,sizeof(gname),nullptr) == 1) {
        f.group_name = gname;
      }
    }
    EVP_PKEY_free(pub);
  }

  // 签名算法：no_name=1 输出 OID 数字串，跨 OpenSSL 版本稳定。
  const  X509_ALGOR* alg = nullptr;
  //只拿：signatureAlgorithm,得到算法描述
  X509_get0_signature(nullptr,&alg,cert.get());

  const ASN1_OBJECT* obj = nullptr;
  if (alg) {
    //从算法结构里拿出它的OID
    X509_ALGOR_get0(&obj, nullptr, nullptr, alg);
  }
  if (obj) {
    char buffer[128];
    // 强制输出数字形式 OID；返回值达到缓冲区长度表示结果被截断。
    const int written = OBJ_obj2txt(buffer, sizeof(buffer), obj, 1);
    if (written > 0 && written < static_cast<int>(sizeof(buffer))) {
      f.signature_oid = buffer;
    }
  }

  // 序列号：位数对标 128 bit，hex 用于比较两次随机结果。
  if (const auto* serial = X509_get_serialNumber(cert.get())) {
    if (auto* bn = ASN1_INTEGER_to_BN(serial, nullptr)) {
      f.serial_bit_length = BN_num_bits(bn);
      char* hex = BN_bn2hex(bn);
      if (hex) {
        f.serial_hex = hex;
        OPENSSL_free(hex);
      }
      for (char& c : f.serial_hex) {
        if (c >= 'A' && c <= 'F') {
          c = static_cast<char>(c - 'A' + 'a');
        }
      }
      BN_free(bn);
    }
  }
  f.not_before = asn1ToTime(X509_get0_notBefore(cert.get()));
  f.not_after = asn1ToTime(X509_get0_notAfter(cert.get()));

  return f;
}

// 验证一个 X.509 证书是否由给定的发行人签名。
bool verifySignedBy(std::string_view cert_pem, std::string_view issuer_pem) {
  X509Ptr cert = pemToX509(cert_pem);
  X509Ptr issuer = pemToX509(issuer_pem);
  if (!cert || !issuer) return false;

  // X509_get_pubkey 会增加引用计数，交给 RAII 释放。
  const PkeyPtr pub(X509_get_pubkey(issuer.get()));
  if (!pub) return false;

  return X509_verify(cert.get(), pub.get()) == 1;
}
// 证书公钥与私钥是否匹配。
bool certMatchesKey(std::string_view cert_pem, std::string_view key_pem) {
  const X509Ptr cert = pemToX509(cert_pem);
  const BioPtr bio = memBio(key_pem);
  if (!cert || !bio) return false;

  const PkeyPtr key(
      PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
  if (!key) return false;

  return X509_check_private_key(cert.get(), key.get()) == 1;
}
}  // namespace chathub::pki
