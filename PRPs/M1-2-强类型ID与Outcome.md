# M1-2 PRP：强类型 ID 与 Outcome

> 状态：已给出待落地（2026-09-01）。实现内容已交付；落地与验证由用户完成，只有用户明确要求“帮我实现 M1-2”时 AI 才代写。

## 1. 目标与范围

在 shared contracts 增加轻量强类型身份／序号（D209）与类型化 `Outcome<T>`（D211），全部 header-only、纯 C++20，不依赖 Qt／Asio／JSON。新增单元测试（无第三方框架，GoogleTest 在 M0 未引入）。

包含：`AccountName`（D44 校验＋规范化）、`MessageId`／`LocalMessageId`／`RequestId`（UUID 8-4-4-4-12）、`DeliverySeq`／`ConversationSeq`（uint64＋规范十进制）、`Outcome<T>`／`Error`（Error.code 为 D128 稳定 snake_case 错误码）。

不做：网络、帧、JSON、Qt；GoogleTest／vcpkg 新依赖；D128 错误码全表的注册表校验（属 M1-3 协议描述）；DeliverySeq／ConversationSeq 的语义范围（水位归 M2／M5）。

## 2. 本步实现决定（记录待 M1-3 对账）

- UUID 解析接受大小写，存储统一小写（避免幂等键大小写分裂）；`MessageId` 的线格式按 UUID 校验，若 M1-3 ProtocolDescriptor 定型不同则只改 parse。
- “规范十进制”实现为：非空、全数字、无前导零、uint64 不溢出。
- `Error.code` 是 `std::string_view`（指向静态字面量）；解析失败走 `std::optional` 空值，不构造错误码。

## 3. 追踪

| 来源 | 条目 |
| --- | --- |
| 设计 | D209（强类型清单与禁止互转）、D211（Outcome／ErrorCode／不抛异常）、D128（错误码字符串合同）、D44（账号名 4—32 字母数字、字母开头、小写规范化）、D13—D15（request_id／local_id／message_id 身份） |
| 路线 | W1 · M1-2 强类型 ID／Outcome（`enum class`、值对象、`std::variant`）；9.4 UUID v4＋规范十进制 |

## 4. 结构变化

- 新增：`shared/include/chathub/contracts/ids.hpp`、`shared/include/chathub/contracts/outcome.hpp`
- 修改：`shared/CMakeLists.txt`（增加 `target_include_directories(... include)`）
- 新增：`tests/contracts_types_test.cpp`；修改：`tests/CMakeLists.txt`（注册 `chathub.contracts.types`，标签 unit;contracts）

## 5. 实施蓝图

```yaml
Task 1: MODIFY shared/CMakeLists.txt — include 目录
Task 2: CREATE shared/include/chathub/contracts/ids.hpp
Task 3: CREATE shared/include/chathub/contracts/outcome.hpp
Task 4: CREATE tests/contracts_types_test.cpp + MODIFY tests/CMakeLists.txt
Task 5: VERIFY — configure/build/ctest；CTest 报告 2/2（smoke + types）
```

## 6. 验证命令与通过标准

```powershell
cmake --preset windows-mingw-debug
cmake --build --preset windows-mingw-debug
ctest --preset windows-mingw-debug
```

通过标准：CTest 报告 2/2 通过、0 失败 0 跳过；types 测试覆盖成功／边界／失败／互转禁止；domain 依赖方向不变（架构断言通过）。

## 7. 风险与停止条件

- 测试只镜像实现是不可接受的红线：每个 CHECK 必须能对应一条合同（D44 边界、D128 格式、D209 互转）；
- GoogleTest 引入前不得为可测试性引入依赖；
- 发现 D128 错误码格式与本实现冲突时停止并回设计确认。

## 8. 完成清单

- [ ] ids.hpp／outcome.hpp 落地且 header-only；
- [ ] contracts target 暴露 include 目录，smoke 仍通过；
- [ ] CTest 2/2 通过；
- [ ] 周文档两处同步。
