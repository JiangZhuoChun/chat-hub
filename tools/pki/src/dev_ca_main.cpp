// 开发用 CA 签发命令：chathub-dev-ca [--ip 127.0.0.1] [--out run-data/pki]。
// 产物写入受 gitignore 的目录；密钥绝不进 Git。

#include <cstdio>
#include <string>

#include "chathub/pki/dev_ca.hpp"

int main(int argc, char** argv) {
  std::string ip{chathub::pki::kDefaultServerIpv4};
  std::string out = "run-data/pki";

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--ip" && i + 1 < argc) {
      ip = argv[++i];
    } else if (arg == "--out" && i + 1 < argc) {
      out = argv[++i];
    } else {
      std::fprintf(stderr,
                   "usage: chathub-dev-ca [--ip 127.0.0.1] [--out dir]\n");
      return 2;
    }
  }

  const auto issued = chathub::pki::issueDevCa(ip);
  if (!issued) {
    std::fprintf(stderr, "error: invalid or failed CA issue for IP '%s'\n",
                 ip.c_str());
    return 1;
  }
  if (!chathub::pki::writeDevCaFiles(*issued, out)) {
    std::fprintf(stderr, "error: cannot write PEM files to '%s'\n",
                 out.c_str());
    return 1;
  }

  std::printf("CA fingerprint (SHA-256): %s\n",
              issued->ca_fingerprint_sha256.c_str());
  std::printf(
      "wrote trusted-ca.pem, ca-key.pem, server-cert.pem, server-key.pem to "
      "%s\n",
      out.c_str());
  return 0;
}