# M1-7 PRP：hello 与 capability 协商

> 状态：自动验证通过（2026-09-03，CTest 9/9）。Task 1 值类型＋capability 交集、Task 2 双端 JSON codec 已交付验证；真实 TLS→frame→hello 状态机集成与 5 秒超时留在 M1-8。

## 1. 目标与范围

在 TLS 成功后、认证前完成首个应用层交换：客户端只发送 `0x04 hello_request`，服务端在 5 秒内验证请求、协商 capability，并以 `0x05 hello_response` 返回固定连接参数。成功后状态转为 `UNAUTHENTICATED`；hello 不访问 MySQL、令牌、认证或业务 Use Case。

本步不做：TCP／TLS 连接、帧编解码、真实 socket Session／写队列／心跳（M1-8）、登录、文件／语音 capability、自动重连、未知内容的业务处理；不修改 D01—D231 正式设计文档。

## 2. 当前事实与授权

| 项目 | 当前事实／边界 |
| --- | --- |
| 现有合同 | `ProtocolType::hello_request=0x04`、`hello_response=0x05` 及 `awaiting_hello` 状态已在 `protocol_descriptor.hpp` 注册；`hello.hpp` 值对象与双端 codec 已交付，由 CTest `chathub.contracts.hello`、`chathub.hello.codec` 覆盖。 |
| 现有网络能力 | M1-6 已验证 TLS 单连接握手；仓库尚无 server dispatcher 或 Session，因此本步不把 socket 操作塞进 hello handler。 |
| 生产代码 | 常规模式下由用户落地；只有用户明确说“帮我实现／修改／修复”才代写。 |
| 测试 | 接口稳定后可补充；测试不得伪造 TLS／帧／状态机已经存在。 |
| Git | 本 PRP 不授权暂存、提交或推送。 |

## 3. 合同与未决项

### 已确定的 wire 合同

帧 type 只存在于 8 字节帧头；JSON 使用 D68 信封、紧凑 UTF-8，正文不超过 65,536 字节：

```json
{"request_id":"<uuid-v4>","data":{"client_version":"1.0","platform":"windows","capabilities":["text_v1"]}}
```

```json
{"request_id":"<uuid-v4>","ok":true,"data":{"server_version":"1.0","server_time":0,"max_json":65536,"max_text":4096,"heartbeat_idle":20000,"timeout":60000,"capabilities":["text_v1"]}}
```

- `capabilities` 是服务端支持集合与客户端声明集合的有序去重交集；首版服务端集合仅 `text_v1`。
- `hello_request` 仅允许在 `AWAITING_HELLO`；首个应用帧不是 hello、重复 hello 或认证后 hello 均按协议错误关闭。
- JSON／结构／类型／长度错误属于协议错误并关闭连接；合法业务失败才使用 D68 的 `ok=false` 响应。
- TLS 成功后开始 5 秒 hello 截止；超时关闭。M1-8 才在真实 Session 中装配该 timer。

### 已确认的三项线协议决定（2026-09-03）

1. `server_time` 单位 = **Unix 秒**，`int64_t`，JSON number（D67）。
2. `client_version ≠ "1.0"` → `protocol_error(unsupported_version, supported_version=1)` 后关闭（D70/D12 永久错误，不用 `ok:false`）；`platform ≠ "windows"` → `protocol_error(invalid_request)` 后关闭。二者由 M1-8 handler 判定。
3. 字段边界：`client_version ≤ 32`、`platform ≤ 16`、capability 单项 `≤ 64`、集合 `≤ 16`；未知 capability 不报错，由交集裁掉（D197）。request_id 为小写 UUID v4（D69）。

## 4. 追踪

| 来源 | 条目 | 本步响应 |
| --- | --- | --- |
| 设计 | D66、D68、D69 | UTF-8 JSON、信封、UUID request_id 与解析顺序。 |
| 设计 | D70—D74 | 帧类型、状态门、hello 5 秒截止、请求超时。 |
| 设计 | D197、D198、D207、D218 | capability 交集、纯 C++ contracts、禁止文件／语音提前实现、注册表。 |
| 周计划 | W2／M1-7 | TLS 后协商、无 MySQL／令牌访问。 |

## 5. 结构与依赖边界

| 文件／目标 | M1-7 责任 | 禁止承担 |
| --- | --- | --- |
| `shared/include/chathub/contracts/hello.hpp` | 纯 C++ DTO、常量、capability 值集合与交集。 | Qt、Asio、Boost.JSON、时间读取、socket、状态转换。 |
| `shared/CMakeLists.txt` | 将公开 hello 头列入 contracts IDE／消费者上下文。 | 引入 Qt／Boost.JSON。 |
| `client-qt/.../hello_json_codec.*`（Task 2 候选） | Qt JSON ↔ `HelloRequest/HelloResponse` 映射。 | 连接、TLS、认证、业务状态。 |
| `chat-server/.../dispatcher/hello_json_codec.*`（Task 2 候选） | Boost.JSON ↔ 纯 DTO 映射；仅在独立 dispatcher adapter target 已确认后创建。 | 把 Boost.JSON 放入 application／domain 或让 handler 接触 socket。 |
| M1-8 Session | 持有 TLS、FrameDecoder、状态、5 秒 timer 和串行写；调用类型化 hello handler。 | 在 M1-7 提前实现。 |

`boost-json` 是服务端 codec 的必要依赖，只有 Task 2 获准时才加入 `vcpkg.json`：vcpkg baseline 锁定版本，许可证为 Boost Software License 1.0，CMake target 以实际 `find_package(Boost COMPONENTS json)` 结果为准。禁止因为 hello 引入第三套 JSON 库。

## 6. API 与状态合同

| 符号／动作 | 输入 | 成功 | 失败／保持 |
| --- | --- | --- | --- |
| `CapabilitySet::intersect` | 两个声明集合 | 稳定有序、无重复交集 | 空集合是合法协商结果。 |
| 客户端 request 编码 | 合法 `HelloRequest`＋UUID | `0x04` 与 D68 请求信封 | 本地字段无效时不发帧。 |
| 服务端 request 解码 | 受限 UTF-8 JSON 字节 | 类型化 `HelloRequest` | 语法／结构／未知必填字段／长度非法：协议错误关闭。 |
| hello handler | `AWAITING_HELLO` 的 DTO | `HelloResponse`，状态转 `UNAUTHENTICATED` | 状态不合法／重复首帧：协议错误关闭；不访问数据库。 |

`request_id` 仅做关联；hello 不允许复用它实现幂等缓存。Capability 协商成功不授权发送 text 业务帧，认证状态仍是进入业务协议的前置条件。

## 7. 实施蓝图

```yaml
Task 1:
  type: MODIFY
  files:
    - shared/include/chathub/contracts/hello.hpp
    - shared/CMakeLists.txt
    - tests/hello_contract_test.cpp
    - tests/CMakeLists.txt
  steps:
    - 以已确认的 server_time 与字段边界收敛现有候选 DTO
    - 保持 contracts 为纯 C++20，并验证 capability 去重、交集和默认连接参数
    - 注册独立 contracts 单测，不改变 ProtocolDescriptor 的既有 type／状态值
  preserve:
    - M1-3 注册表与 M1-4 FrameDecoder 行为
    - 不引入 JSON、Qt、Asio、Boost 或文件／语音实现
  failure:
    - 未决线协议项未确认时停止，不创建猜测性的默认语义
  done_when:
    - hello 值对象与合同测试可由 contracts 消费者独立编译运行

Task 2:
  type: CREATE
  files:
    - client-qt 的 Qt JSON hello codec
    - chat-server dispatcher adapter 的 Boost.JSON hello codec
    - 对应 codec 测试与最小 CMake/vcpkg 依赖变更
  steps:
    - 先确认 server dispatcher target 的依赖白名单和 Boost::json 实际 target
    - 严格按 D66 顺序解析，并映射 D68 信封与纯 DTO
    - 客户端和服务端 codec 分别测试；不共享 Qt／Boost JSON 类型
  preserve:
    - application/domain 不依赖 JSON；transport 不直接承担业务处理
  failure:
    - 解析库 target、线协议未决项或依赖边界不明确时停止并确认
  done_when:
    - 双 codec 对同一合法 fixture 产生等价 DTO；非法 JSON／根／字段／长度被拒绝

Task 3:
  type: DESIGN
  files:
    - PRPs/M1-7-hello与capability.md
    - M1-8 Session PRP（后续）
  steps:
    - 明确 M1-8 将如何把 TLS 完成事件切至 AWAITING_HELLO
    - 明确 protocol_error 的发送后关闭、hello deadline 和状态代次归属
    - 仅在 M1-8 实现时做真实 TLS → frame → hello 集成测试
  preserve:
    - M1-7 不创建半成品 Session、写队列或心跳
  failure:
    - 若需要 socket／timer 所有权，转入 M1-8，不在本 Task 提前编码
  done_when:
    - M1-8 有可执行的集成装配合同，M1-7 不伪称已完成端到端握手
```

## 8. 测试与验证计划

| 层级 | 场景 | 关键断言 |
| --- | --- | --- |
| contracts 单测 | 空／重复／未知 capability，双方交集 | 有序去重；未知 capability 不会被服务端回显；空交集合法。 |
| contracts 单测 | 默认 `HelloResponse` | 版本与 `max_json=65536`、`max_text=4096`、`heartbeat_idle=20000`、`timeout=60000` 对齐已确认合同。 |
| codec 单测 | 合法 request／response | 两端各自 JSON codec ↔ DTO 无损；`request_id` 与 type 配对正确。 |
| codec 负例 | 非 UTF-8、JSON 语法、非对象、未知必填字段、错误类型、超长字段 | 不创建 DTO；按协议错误路径返回给上层。 |
| M1-8 集成 | TLS 后首帧 hello／非 hello／重复 hello／5 秒无 hello | 成功才进入 unauthenticated；其余发送 protocol_error（适用时）后关闭；无 MySQL 调用。 |

## 9. 验证命令

Task 1／Task 2 的实际测试 target 创建后，使用活动 CLion 配置 `windows-mingw-debug-local`：

```powershell
cmake --build out/build/windows-mingw-debug --target <hello-target>
ctest --test-dir out/build/windows-mingw-debug --output-on-failure
```

预期：构建和 CTest 退出码均为 0；必须单列 Passed／Failed／Skipped。M1-8 前不把 CTest 通过表述为“真实 TLS 后首帧已验证”。

## 10. 文档同步与停止条件

- 当前交付同步本 PRP、`docs/周计划/W2-需求与验收.md` 与 `W2-功能代码对照.md`；不修改正式设计文档。
- 实施前出现线协议字段、错误语义、时间单位或依赖 target 争议：停止并请用户确认。
- 发现待提交 `hello.hpp` 与确认合同冲突：保留用户改动，报告差异，不覆盖。
- 不新增文件／语音、认证、数据库、Session、心跳或自动重连；它们不属于 M1-7 的实现授权。

## 11. 完成清单

- [x] M1-7 目标、边界、依赖方向与 M1-8 分界已规划；
- [x] 当前候选 `hello.hpp` 与未决线协议项已记录；
- [ ] 三项线协议决定已确认；
- [ ] Task 1 合同与单测已落地；
- [ ] Task 2 codec 与依赖边界已落地；
- [ ] M1-8 真实 TLS／状态机集成验证已完成；
- [x] 周文档已同步为“已给出待落地”。
