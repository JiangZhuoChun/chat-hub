# M1-1 PRP：客户端与服务端分层 CMake 骨架

> 状态：已给出待落地（2026-09-01）。实现内容已交付；落地与验证由用户完成，只有用户明确要求“帮我实现 M1-1”时 AI 才代写。

## 1. 目标与范围

按 D199／D208 建立客户端与服务端的分层 CMake target 骨架，依赖方向由 configure 期断言强制（D221 最小版）。

- 服务端：`chathub::server::domain`／`application`／`infrastructure`／`transport`
- 客户端：`chathub::client::domain`／`application`／`infrastructure`／`presentation`
- 分层 target 先用 INTERFACE（零源码、零占位，符合 D207）；M1-2 向 domain 落首批代码时转 STATIC。
- app／bootstrap target 延后到首个需要 main 的能力点（M1-6 TLS 双端或 M1-7 hello）。

不做：Qt／Asio／OpenSSL 依赖引入（M1-5 起）；强类型 ID（M1-2）；vcpkg 依赖变更；空 handler／空状态机占位（D207）。

## 2. 状态与授权

| 当前状态 | 已给出待落地 |
| --- | --- |
| 生产代码 | 用户落地 AI 给出的内容 |
| 架构断言 | 属构建配置，随本步交付 |
| Git | 提交需用户明确授权 |

## 3. 追踪

| 来源 | 条目 |
| --- | --- |
| 路线 | W1 · M1-1 CMake 分层（static library、PUBLIC／PRIVATE） |
| 设计 | D199（依赖方向）、D208（target 边界与命名权）、D214（纵向切片）、D221（CMake＋架构检查强制、最小 PUBLIC／PRIVATE） |

## 4. 上下文

- `AGENTS.md`：分层红线（第 5 节）；
- `docs/周计划/W1-需求与验收.md`：当前能力点；
- 详细设计 D199／D208／D221 原文；
- 现有根 `CMakeLists.txt`（`include(CTest)`、`shared`、`tests`）与 `tests/CMakeLists.txt`（smoke 注册）。

## 5. 结构变化

- 新增：`chat-server/CMakeLists.txt`、`client-qt/CMakeLists.txt`
- 修改：根 `CMakeLists.txt`（挂载两个子目录）、`tests/CMakeLists.txt`（追加架构断言）
- `tests/` 之后新增的构建骨架目录为 `chat-server/`、`client-qt/`。

## 6. 直接链接白名单（断言依据）

| target | 允许直接链接 |
| --- | --- |
| `chathub::server::domain` | （无） |
| `chathub::server::application` | `chathub::server::domain` |
| `chathub::server::infrastructure` | `chathub::server::domain` |
| `chathub::server::transport` | `chathub::server::application` |
| `chathub::client::domain` | （无） |
| `chathub::client::application` | `chathub::client::domain` |
| `chathub::client::infrastructure` | `chathub::client::domain` |
| `chathub::client::presentation` | `chathub::client::application` |

## 7. 风险与停止条件

- 断言只检查 INTERFACE target 的直接依赖；传递依赖与头文件扫描在 M1-4 完善；
- M1-2 把 INTERFACE 转 STATIC 时必须保留 alias 与白名单；
- 发现 D208 命名与本骨架冲突时停止并确认。

## 8. 实施蓝图（同一时间只执行一个 Task）

```yaml
Task 1:
  type: CREATE
  files: [chat-server/CMakeLists.txt]
  done_when: 4 个服务端 INTERFACE target＋alias＋cxx_std_20＋白名单链接

Task 2:
  type: CREATE
  files: [client-qt/CMakeLists.txt]
  done_when: 4 个客户端 INTERFACE target，presentation 顶层

Task 3:
  type: MODIFY
  files: [CMakeLists.txt]
  steps: [在 add_subdirectory(tests) 之后挂载 chat-server 与 client-qt]

Task 4:
  type: MODIFY
  files: [tests/CMakeLists.txt]
  steps: [追加 chathub_check_direct_links 函数与 8 条断言]

Task 5:
  type: VERIFY
  done_when: configure/build/ctest 退出码 0；smoke 仍 1/1；负向验证触发 FATAL_ERROR 后还原
```

## 9. 验证命令

```powershell
cmake --preset windows-mingw-debug
cmake --build --preset windows-mingw-debug
ctest --preset windows-mingw-debug
```

负向验证：临时给 `chathub_server_domain` 添加 `target_link_libraries(chathub_server_domain INTERFACE chathub::contracts)` → configure 必须 FATAL_ERROR 报“架构违规”→ 还原后重新 configure 通过。

## 10. 文档同步

交付后更新 `docs/周计划/W1-功能代码对照.md` 对应行为“已交付”，并刷新 `W1-需求与验收.md` 实时状态。

## 11. 完成清单

- [ ] 8 个 target 与别名按白名单建立；
- [ ] configure／build／ctest 退出码 0，smoke 仍 1/1；
- [ ] 负向验证 FATAL_ERROR 触发并还原；
- [ ] 周文档两处同步。
