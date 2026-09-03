# M1-3 PRP：协议描述 ProtocolDescriptor

> 状态：已交付（2026-09-01）。首验失败两处均修正：① `hasState` 状态序号误当位掩码（用户定位）；② 一条断言期望写反（已认证集合不应包含 awaiting_hello）。修正后干净目录 CTest 3/3 退出码 0；保留 type nullptr 测试与说明性注释已补。

## 1. 目标与范围

在 shared contracts 建立只读 ProtocolDescriptor 注册表（D218）：静态 `constexpr` 数据，覆盖设计合同 §8“当前已分配的类型”全部 **54 个类型**（`0x01`—`0x87`；未列出范围保持预留，不得自行占用）。字段：type、名称、方向、允许连接状态、capability、最大正文、默认超时、响应 type。

不做：帧编解码（M1-4）、JSON、Qt／Asio、错误码注册表、capability 协商逻辑（hello 交换属 M1-7）。

## 2. 字段取值决策（全部来自合同；PRP 自行决定处已标注）

| 字段 | 取值 | 依据 |
| --- | --- | --- |
| type／名称 | 设计合同 §8 类型全表，逐字照录 | §8“当前已分配的类型” |
| 方向 | request（C2S 请求）／response（配对响应）／push（S2C 无配对）／bidirectional（heartbeat） | §8 表“方向”列 |
| 允许连接状态 | hello 仅 `awaiting_hello`；注册、登录、恢复、重置仅 `unauthenticated`；heartbeat → `unauthenticated \| authenticated`；protocol_error → 全状态；其余（业务＋push）→ `authenticated` | D72 状态机 |
| 默认超时 | hello 5s；注册 prepare／finalize、登录、恢复会话、重置密码 10s；历史、联系人、申请列表 10s；发送聊天 5s；普通好友、资料、清空、阅读进度 5s；退出 2s；响应／推送／心跳／protocol_error = 0 | D74、D73 |
| `delivery_progress`（0x52） | 5000ms——D74 未列明，按轻量请求归 5s 档（**实现决定，M5 对账**） | 本步决定 |
| `conversation_list`（0x54） | 10000ms——响应体最重的读请求，与历史同档（**实现决定，M5 对账**） | 本步决定 |
| capability | `message_send`／`message_received_push` = `text_v1`；其余 `""`（base） | D197（text_v1／file_v1／voice_v1） |
| 最大正文 | 全部 65,536（D09 全局上限）；heartbeat = 0（空正文，D72） | D09 |

## 3. 追踪

| 来源 | 条目 |
| --- | --- |
| 设计 | D218（注册表字段与测试要求）、D72（状态机）、D73（建连超时）、D74（请求超时）、D197（capability）、D09（64 KiB） |
| 路线 | W1 · M1-3 协议描述 |

## 4. 结构变化

- 新增：`shared/include/chathub/contracts/protocol_descriptor.hpp`（`ProtocolType` 54 项、`Direction`、`ConnectionState(s)`、`ProtocolDescriptor`、`kProtocolDescriptors`、`findProtocol`）
- 新增：`tests/contracts_protocol_test.cpp`；修改：`tests/CMakeLists.txt`（注册 `chathub.contracts.protocol`，标签 unit;contracts）

## 5. 验证命令与通过标准

```powershell
cmake --preset windows-mingw-debug
cmake --build --preset windows-mingw-debug
ctest --preset windows-mingw-debug
```

通过标准：CTest **3/3** 通过、0 失败 0 跳过；测试覆盖 type／name 唯一、请求响应配对（每个 request 的 response 存在且仅被一个 request 引用）、状态非空且逐类合法、`max_body ≤ 65536`、请求超时＞0 而响应／推送为 0、capability 决策抽查。架构断言不受影响。

## 6. 风险与停止条件

- 类型号是永久线协议合同：录入后不得改值；发现与设计 §8 不一致立即停止；
- 与 M1-2 实现决定的对账点：`MessageId` 线格式、UUID 小写规范化——在 M1-4 codec 落地前确认；
- 未列出的 type 范围（0x03、0x06—0x0F、0x1E—0x2F 等）保持预留，`findProtocol` 返回 `nullptr` 供 M1-4 判“未知 type”。

## 7. 完成清单

- [x] protocol_descriptor.hpp 落地，54 项与设计 §8 逐字一致；
- [x] CTest 3/3 通过；
- [x] 周文档两处同步。

## 8. 本轮检查证据

```text
CLion: chathub_contracts_protocol_test -> exit 0
CLion: 所有 CTest -> 3/3 passed, 0 failed, 0 skipped, exit 0
```

已修复的根因：`ConnectionState` 使用状态序号（0／1／2），`ConnectionStates` 使用位掩码（1／2／4）；`hasState` 现先左移生成掩码后再判断。

本轮复查已收口：已删除行末反斜杠；已补 `authenticated` 状态位正反断言；已验证每个已登记 type 可被 `findProtocol` 找到，保留 `0x03` 返回 `nullptr`。本次另为 54 个 ProtocolType 枚举项和新增测试边界补充说明性注释，未改变协议行为。
