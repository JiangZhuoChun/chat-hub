# M1-5 PRP：私有 CA

> 状态：已给出待落地（2026-09-02）。实现内容已列出；落地与验证由用户完成。

## 1. 目标与范围

为开发环境建立独立私有根 CA 与服务器叶证书（D06、D125、D126、D127），并引入锁定的 OpenSSL 3.5（D63）。本步只做签发、PEM 落盘、合同检查；不握手、不改 Windows 全局信任库。

不做：`QSslSocket`／`asio::ssl::stream`（M1-6）；hello（M1-7）；Session／心跳（M1-8）；DPAPI 加密 PKCS#8 与服务 SID ACL（M2-2／D123）；`chathub-admin.exe` 正式 PKI（D172／M7）；CRL／OCSP；证书热切换。

## 2. 行为合同

| 场景 | 行为 | 依据 |
| --- | --- | --- |
| 签发开发 CA | ECDSA P-256＋SHA-256；根 10 年；叶 1 年 | D126、D125 |
| 根证书扩展 | CA＝true，keyCertSign，cRLSign | D126 |
| 服务器证书扩展 | CA＝false，仅 digitalSignature＋serverAuth；SAN 只有给定 IPv4，无 DNS／IPv6 | D126、D61 |
| 序列号 | 非零，至少 128 bit 随机 | D126 |
| 非法 IPv4 | 拒绝签发，不写文件 | D60、D61 |
| 信任包 | 只输出 CA **公钥** PEM（`trusted-ca.pem`）；根私钥单独文件且 gitignore | D06、D127、D125 |
| OpenSSL 归属 | 仅 `chathub::pki` 链接 `OpenSSL::SSL`／`OpenSSL::Crypto`；domain／application 不链接 | D63、D221 |

实现决定：开发私钥用未加密 PEM（加密 PKCS#8＋DPAPI 留 M2）；默认 SAN IP＝`127.0.0.1`（D60）；指纹为证书 DER 的 SHA-256 小写十六进制、无冒号。

## 3. 追踪

| 来源 | 条目 |
| --- | --- |
| 设计 | D06（应用内私有 CA）、D61（证书含具体 IPv4）、D63（OpenSSL 3.5 LTS／vcpkg）、D125—D127（根／叶／信任包）、D208（target 边界） |
| 路线 | W2 · M1-5 私有 CA |

## 4. 结构变化

- 新增：`tools/pki/CMakeLists.txt`、`tools/pki/include/chathub/pki/dev_ca.hpp`、`tools/pki/src/dev_ca.cpp`、`tools/pki/src/dev_ca_main.cpp`、`tests/pki_dev_ca_test.cpp`
- 修改：根 `CMakeLists.txt`、`vcpkg.json`、`tests/CMakeLists.txt`

## 5. 验证命令与通过标准

```powershell
cmake --preset windows-mingw-debug
cmake --build --preset windows-mingw-debug
ctest --preset windows-mingw-debug
```

通过标准：CTest **5/5** 通过、0 失败 0 跳过（在既有 4 个 contracts 测试之上新增 `chathub.pki.dev_ca`）。首次 configure 会编译 OpenSSL，耗时可能数分钟。

## 6. 风险与停止条件

- 当前 vcpkg baseline `30ef65c` 默认 OpenSSL 为 **3.6.4**，与 D63「3.5 LTS」冲突；本步用 override 钉 **3.5.4**（该 baseline 版本库中最新的 3.5；**没有 3.5.8**）。若 override 在 `x64-mingw-dynamic` 上构建失败，停止，不擅自改用 3.6。
- 发现签发结果与 D126 扩展不一致时停止。
- 私钥／PEM 进入 Git 时停止并移出。

## 7. 完成清单

- [ ] vcpkg 锁定 OpenSSL 3.5.4 且 PKI target 链接成功；
- [ ] 签发／检查 API 与 `chathub-dev-ca` CLI 落地；
- [ ] CTest 5/5 通过；
- [ ] 周文档两处同步。
