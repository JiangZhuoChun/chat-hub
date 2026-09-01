# M1-4 PRP：安全 FrameDecoder

> 状态：已给出待落地（2026-09-01）。实现内容已交付；落地与验证由用户完成，只有用户明确要求“帮我实现 M1-4”时 AI 才代写。

## 1. 目标与范围

在 shared contracts 建立帧层（D07—D09、D70）：8 字节帧头常量（magic `CH`＝0x43 0x48、version=1）、`FrameDecoder` 流式解码（半包／粘包统一处理、失败即锁存）、`encode_frame` 双向编码、合法 UTF-8 校验。全部纯 C++20 header-only。

不做：UTF-8 之外的 JSON 解析；`protocol_error` 的发送与连接关闭动作（属 Session，M1-6／M1-7）；GoogleTest；头文件扫描式架构检查（后续完善）。

## 2. 行为合同

| 场景 | 行为 | 依据 |
| --- | --- | --- |
| 帧头未收齐 | 保持缓冲，不产出 | D08 接收顺序 |
| magic ≠ `CH` | `bad_magic` 锁存，不扫描重同步 | D70 |
| version ≠ 1 | `bad_version` 锁存（Session 发 protocol_error 后关闭，客户端停止重连） | D70、D12 |
| 未注册 type | `unknown_type` 锁存 | D11 |
| `body_length` > 该 type 的 `max_body` | 分配前拒绝（`body_too_large`），构造 4 GiB 长度也只喂帧头即失败 | D09 |
| 正文非法 UTF-8 | `invalid_utf8` 锁存（含代理区／超长编码／> U+10FFFF） | D11、接收顺序 |
| heartbeat | `max_body=0`，任何正文都拒绝 | D72 |
| 解码器失败后 | 不再产出、不再接受输入；连接必须关闭 | D11 |

实现决定：`feed()` 结束时压缩已消费前缀，防止长连接下缓冲无界增长。

## 3. 追踪

| 来源 | 条目 |
| --- | --- |
| 设计 | D07—D09（帧结构与上限）、D11（协议错误关闭）、D70（magic／version／不重同步）、D218（`find_protocol` 判未知 type） |
| 路线 | W1 · M1-4 安全 FrameDecoder |

## 4. 结构变化

- 新增：`shared/include/chathub/contracts/frame.hpp`、`tests/frame_decoder_test.cpp`
- 修改：`tests/CMakeLists.txt`（注册 `chathub.contracts.frame`，标签 unit;contracts）

## 5. 验证命令与通过标准

```powershell
cmake --preset windows-mingw-debug
cmake --build --preset windows-mingw-debug
ctest --preset windows-mingw-debug
```

通过标准：CTest **4/4** 通过、0 失败 0 跳过；场景覆盖半包（逐字节喂入）、粘包（两帧一次）、空正文 heartbeat、4 GiB 超长、非法 magic／version、未知 type、非法 UTF-8（代理区）、encode↔decode 往返。

## 6. 风险与停止条件

- UTF-8 校验表必须拒绝：代理区（0xED 0xA0—0xBF）、超长编码（0xC0／0xC1、0xE0 0x80—0x9F、0xF0 0x80—0x8F）、> U+10FFFF（0xF4 0x90+）；
- `body_length` 校验必须发生在任何分配之前；
- 发现帧常量与 D70 不一致时停止并回设计确认。

## 7. 完成清单

- [ ] frame.hpp 落地（常量／校验／解码器／编码器）；
- [ ] CTest 4/4 通过；
- [ ] 周文档两处同步。
