# M1-6 PRP：TLS 双端

> 状态：进行中（2026-09-03）。Task 1（服务端 `TlsContext`）自动验证通过；Task 2（客户端 `TlsClientConfig`）已给出待落地；Task 3（真实 TLS 集成）仅规划。

## 1. 目标与范围

为控制长连接建立双向 TLS（D05、D62、D63、D73、D126）。服务端用 `asio::ssl::stream<tcp::socket>` 加载证书链与私钥并完成服务端握手；客户端用 `QSslSocket` 只信任项目私有 CA、启用对端验证并校验服务器 IP。握手成功才发业务帧，失败不降级明文、不 `ignoreSslErrors()`。

不做：hello 首帧（M1-7）；Session 状态机／超时／心跳（M1-8）；DPAPI 加密私钥（M2-2）；客户端证书（D126 首版不用）；IPv6（D61）。

## 2. 拆分与验收

| Task | 内容 | 验证 |
| --- | --- | --- |
| Task 1（本步） | 服务端 `TlsContext`：asio ssl context 加载证书链＋私钥，TLS 1.2 下限 | 单测：有效证书加载成功；缺文件／坏私钥失败 |
| Task 2 | 客户端 `TlsClientConfig`：OpenSSL 后端检查＋只信任私有 CA＋VerifyPeer | build 通过；CA 加载／后端可用并入 Task 3 |
| Task 3 | asio 服务端 ↔ QSslSocket 客户端真实 TLS 集成＋握手逻辑 | 集成：握手成功；证书链失败拒绝；IP 不匹配拒绝；不降级明文 |

## 3. 行为合同（D62／D63／D73）

| 场景 | 行为 | 依据 |
| --- | --- | --- |
| 服务端加载 | 共享只读 ssl::context，加载证书链＋私钥；失败即拒绝启动 | D62 |
| 服务端握手 | 每连接独立 ssl::stream，async_handshake(server) 成功后才读帧 | D62 |
| 客户端信任 | 只加载项目私有 CA；VerifyPeer；校验对端 IP | D62、D61 |
| 客户端失败 | sslErrors／后端缺失／CA 缺失／IP 不匹配 → 永久连接错误，不 ignoreSslErrors | D62 |
| 超时 | TCP 5 秒、TLS 5 秒 | D73 |
| TLS 版本 | 最低 1.2、优先 1.3；1.2 仅 ECDHE＋AEAD | D126 |
| OpenSSL | 统一 3.5 LTS（vcpkg）；后端缺失／版本不匹配即阻止 | D63 |

## 4. 结构变化

- Task 1：`vcpkg.json` 加 `asio`；`chat-server/CMakeLists.txt` transport 链 asio＋OpenSSL＋contracts；新增 `chat-server/include/chathub/server/transport/tls_context.hpp`、`tests/tls_context_test.cpp`
- Task 2：`client-qt/CMakeLists.txt` 加 Qt6::Network；新增客户端 TLS 封装
- Task 3：集成测试 target

## 5. 验证命令与通过标准

```powershell
cmake --preset windows-mingw-debug
cmake --build --preset windows-mingw-debug
ctest --preset windows-mingw-debug
```

Task 1 通过：CTest 6/6（既有 5 + 新增 `chathub.server.tls_context`）；首次 configure 会编译 asio（header-only，较快）。

2026-09-03 验证：CLion 活动配置 `windows-mingw-debug-local`、Qt-MinGW-13.1；“所有 CTest”退出码 0，6/6 通过、0 失败、0 跳过。

## 6. 风险与停止条件

- asio 为 standalone（禁止 Boost.Asio），由 vcpkg `asio` port 提供。若 `find_package(asio)` 的 target 名与假设不符（`asio` 与 `asio::asio` 二选一），按 vcpkg 报错修正，不引入 Boost。
- OpenSSL 3.5.4 已在 M1-5 锁定；客户端 QSslSocket 后端必须是同一 vcpkg 树的 OpenSSL（D63）。
- 若 `ssl::context::no_tlsv1` 等选项在 OpenSSL 3.5 下编译失败／告警，改用 `SSL_OP_` 位或移除（3.5 默认已禁 TLS<1.2）。
- 任何“握手失败仍发业务数据”或调用 `ignoreSslErrors()` 即停止。

## 7. 追踪

| 来源 | 条目 |
| --- | --- |
| 设计 | D05（TLS 不降级）、D61（证书含 IP）、D62（QSslSocket／asio::ssl）、D63（OpenSSL 3.5）、D73（超时）、D126（TLS 版本／证书） |
| 路线 | W2 · M1-6 TLS 双端 |

## 8. 完成清单

- [x] Task 1 服务端 `TlsContext`（证书加载）＋单测；
- [ ] Task 2 客户端 `QSslSocket` TLS 封装；
- [ ] Task 3 真实 TLS 集成测试（握手／拒绝／不降级）；
- [ ] 周文档同步。