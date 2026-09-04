# M1-8 PRP：TLS Session、hello 状态门与超时

> 状态：已给出待落地（2026-09-04）。本 PRP 规划 M1-8 的连接生命周期；不把登录、认证、重连或业务 handler 提前实现。Git 暂存、提交和推送不在本 PRP 授权内。

## 1. 目标与非目标

在 M1-6 TLS 握手成功后装配一条服务端 `Session`，以真实 TLS 字节流驱动 M1-4 帧层和 M1-7 hello codec，实现下列可达状态：

```text
TLS handshake success
  -> awaiting_hello --合法 hello--> unauthenticated --关闭--> closing
```

`authenticated` 是协议状态机的既有后续状态：M1-8 预留其计时和心跳接入点，但没有认证 handler 时不可由真实客户端进入，不能伪造为已端到端验证。

本步完成：TLS 读循环、帧状态门、hello 版本／平台／capability 判断、串行写队列、hello／未认证截止、协议错误“写完后关闭”、D55 的认证后心跳计时逻辑和真实 TLS→frame→hello 集成测试。

本步不做：登录／注册／令牌／MySQL／Redis、authenticated 业务 handler、客户端重连或登录页、通用 handler 注册表、文件／语音、生产服务器 acceptor／可执行程序、认证后真实业务推送。未来 Composition Root 在 accept 成功和 M1-6 `asyncServerHandshake` 回调成功后构造并 `start()` Session。

## 2. 先决条件与已发现的合同冲突

### 2.1 M1-7 必须先修正

M1-8 不得把以下 M1-7 问题带入真实网络入口；它们可以作为同一后续修复提交的 Task 0，但必须先有测试：

1. `request_id` 必须只接受小写 UUID v4（D69）；现有通用 `UuidId::parse` 仅校验 UUID 外形，现有 hello 测试样本还是 UUID v1。
2. 两端 codec 在 JSON 解析／分配前必须保证正文不超过 `kMaxJsonBytes`；Session 的 FrameDecoder 上限是必要前置条件，codec 自身也应拒绝其公开 API 的超长输入。
3. 客户端 `encodeHelloRequest` 必须拒绝本地非法 DTO，而不能发送空 request_id、空版本或越界 capability。
4. 服务端 Boost.JSON codec 从 `transport` 迁到本 PRP 的 dispatcher target；其公开头不暴露 Boost 类型，不应把 Boost.JSON 作为 transport 的公开编译依赖。

### 2.2 本 PRP 的明确解释

| 问题 | 结论 |
| --- | --- |
| `ConnectionState` 没有 `closing` | 不修改共享协议位图。Session 持有既有 `ConnectionState` 作为可收帧状态，另有私有 `closing_` 生命周期标志；进入关闭后不再读、分发或重设计时器。 |
| D200 要求 Dispatcher，但本步不建注册表 | 新增薄的 `ProtocolDispatcher` target，内部只有显式 `switch`，不创建 map／factory／可配置注册表。它负责 JSON codec、类型化 hello handler 和 protocol_error JSON；Session 不接触原始 JSON。 |
| `unsupported_version` 的版本字段 | 帧头版本不兼容（D70）使用整数 `supported_protocol_version: 1`；hello 的 `client_version="1.0"` 不兼容使用字符串 `supported_client_version: "1.0"`。两者不能共用含义不明的 `supported_version=1`。 |
| 认证后心跳测试 | M1-8 没有认证 handler，真实连接不能进入 `authenticated`。M1-8 只验证 hello／未认证 60 秒截止；认证后 20/60 秒计时分支由 M2 首个认证成功集成测试验证，不以 test hook 伪造生产认证。 |
| 写队列 256 帧／2 MiB | M1-8 仅产生 hello、heartbeat 和一次 protocol_error，小响应不能真实填满队列。因此本步实现并做内部计数边界单测；“慢客户端真实触发上限”留到 M2 有可持续业务／推送输出后，不能写为 M1-8 集成通过。 |

若用户要求 `client_version` 不兼容仍使用数值 `supported_version=1`，必须先显式改写上述 wire 合同和 M1-7 的 `kSupportClientVersion="1.0"`，再实施。

## 3. 已有装配点

| 环节 | 已交付事实 | M1-8 接入 |
| --- | --- | --- |
| TLS | `asyncServerHandshake` 成功时交出 `std::shared_ptr<SslStream>` | Composition Root／测试 accept 回调构造 `Session` 并调用 `start()`。 |
| 帧 | `FrameDecoder::feedBytes`／`tryPopFrame`、`encodeFrame` | 读回调喂入字节，逐帧分发；decoder 失败映射为关闭或 protocol_error。 |
| hello | 服务端 Boost.JSON codec、客户端 Qt JSON codec、`HelloRequest/Response` | codec 迁入 dispatcher；handler 只接收已类型化 `HelloRequest`。 |
| 状态 | `awaiting_hello`、`unauthenticated`、`authenticated` 及 `findProtocol` | Session 先用 descriptor 检查 `allowed_states`，再调用显式分支。 |
| TLS 测试模式 | `tls_integration_test.cpp` 已有 loopback acceptor、私有 CA、QSslSocket 与 IO 线程守卫 | 复用其证书、线程和 Qt 事件循环模式；测试不信任旧二进制。 |

## 4. 线协议与状态合同

### 4.1 读／分发顺序

1. `async_read_some` 只在 Session strand 上启动；每轮读取固定 8 KiB 缓冲。
2. 把收到字节喂给 `FrameDecoder`；`bad_magic` 直接关闭，`bad_version` 在尚可识别时发送 `protocol_error` 后关闭，其余 decoder 失败按 `protocol_error` 后关闭。
3. 对每个完整 `Frame`，先用 `findProtocol(frame.type)` 与 `hasState(descriptor->allowed_states, state_)` 检查状态；不允许时发送 `invalid_connection_state` 后关闭。
4. 在允许状态内只实现：`hello_request`（awaiting_hello）和空正文 `heartbeat`（unauthenticated／authenticated）。其他已登记类型返回 `unsupported_type` protocol_error 后关闭；不因“已经登记”而假装 handler 已存在。
5. 只有通过帧解析、状态门和已实现 handler 的入站帧才刷新活动时间；TLS 字节、垃圾、超长帧和错误状态帧不刷新。

### 4.2 hello handler（纯 C++20 application）

`HelloHandler` 输入 `HelloRequest` 和由 Composition Root／Session 采集的当前 Unix 秒，输出成功 `HelloResponse` 或受控拒绝枚举；它不接触 socket、JSON、定时器或数据库。

| 输入 | 结果 | Session 动作 |
| --- | --- | --- |
| `client_version != "1.0"` | `unsupported_client_version`，`supported_client_version="1.0"` | protocol_error 写完后关闭 |
| `platform != "windows"` | `invalid_request` | protocol_error 写完后关闭 |
| 合法请求 | `capabilities = {text_v1} ∩ request.capabilities`；填当前秒级 `server_time` 与既有连接常量 | 编码 `hello_response`；成功入队后状态转 `unauthenticated`，重设计时器为首次认证 60 秒 |

`hello_response` 进入写队列即为状态转换的线性化点；之后任何第二个 hello 都因状态门失败而关闭。不得在入队失败时转到 `unauthenticated`。

### 4.3 protocol_error

`protocol_error` 是服务端推送（无 `request_id`／`ok`），正文统一为紧凑 UTF-8 JSON：

```json
{"code":"invalid_connection_state","message":"当前连接状态不允许该帧","retryable":false}
```

仅以下情形追加固定、已知字段：帧头版本不兼容追加整数 `supported_protocol_version`；hello 客户端版本不兼容追加字符串 `supported_client_version`。错误正文不得回显请求正文、token、证书或内部错误。

`bad_magic` 直接关连接、不发响应；其余可识别协议错误调用 `sendProtocolError`，设置 `closing_`，停止新读和 timer，保留当前写帧，最后一帧 `async_write` 成功／失败后关闭 lowest layer。关闭路径幂等，只允许一个 protocol_error。

## 5. 定时器、心跳和写队列

### 5.1 单 timer／代次

Session 只有一个 `asio::steady_timer` 和单调 `timer_generation_`。每次 `expires_after` 前递增代次并捕获 `{generation, expected_state, deadline_kind}`；回调仅在没有 `operation_aborted`、尚未 `closing_`、代次和状态都一致时生效。每次状态转换或有效入站活动都取消并重新安排下一截止。

| 状态 | 截止 | 触发结果 |
| --- | --- | --- |
| awaiting_hello | 从 `start()` 起 5 秒 | 直接关闭；没有可关联 request_id，不发业务失败响应。 |
| unauthenticated | hello 成功后／认证失败后 60 秒 | 关闭；入站 heartbeat 不延长该期限（D73）。 |
| authenticated（M2 进入后） | 空闲 20 秒发送空正文 heartbeat；连续 60 秒无有效入站帧关闭 | 入站业务帧／heartbeat 刷新活动时间；仅 M2 集成验证真正进入本状态。 |

“空闲”以有效入站活动为准；认证后若 20 秒没有活动，发送一次 heartbeat，并继续以 60 秒入站静默上限为硬截止。所有 timer 回调均经 Session strand 串行化。

### 5.2 写队列

使用 `std::deque<std::vector<std::uint8_t>>`，维护 `queued_frames_` 与 `queued_bytes_`。每次入队先检查新增帧后的数量 `<= 256` 且字节 `<= 2 MiB`；只允许一个在途 `async_write`。写完成后先扣账、弹出，再启动下一帧。

达到上限时不再尝试塞入 protocol_error：以 `client_too_slow` 关闭，避免错误帧本身越过上限。`sendProtocolError` 在正常容量内保证错误帧排在既有队列后，并在该帧完成后关闭。

## 6. 文件、目标与依赖

| 位置 | 责任 | 依赖边界 |
| --- | --- | --- |
| `shared/include/chathub/contracts/protocol_error.hpp`（候选） | 最小协议错误码／字段值对象；若 dispatcher 内部枚举即可表达，则不创建公共头。 | 纯 C++20。 |
| `chat-server/include/chathub/server/application/hello_handler.hpp`、`src/hello_handler.cpp` | 版本、平台、capability 交集、秒级时间输入。 | 仅 contracts／C++20。 |
| `chat-server/include/chathub/server/dispatcher/{hello_codec,protocol_dispatcher}.hpp`、`src/...cpp` | 迁移 Boost.JSON hello codec；显式 switch；编码 protocol_error。 | contracts、application、Boost.JSON；无 Asio/socket。 |
| `chat-server/include/chathub/server/transport/session.hpp`、`src/session.cpp` | TLS 流、读缓冲、FrameDecoder、状态门、单 timer、写队列、连接 identity 和关闭。 | dispatcher、Asio、OpenSSL、contracts；无 JSON／业务规则。 |
| `chat-server/CMakeLists.txt` | application／dispatcher 由 INTERFACE 转为含真实 `.cpp` 的 STATIC target；transport 链接 dispatcher。 | `Boost::json` 仅 dispatcher；不传递到 transport 公共头。 |
| `tests/hello_integration_test.cpp` | 真实 TLS 后 hello 成功、首帧非法、版本失败、5 秒 deadline。 | loopback TLS，不 mock TLS／FrameDecoder。 |
| `tests/session_timeout_test.cpp` | 未认证 deadline、timer 代次、写队列账本边界。 | 不虚构认证业务或持续业务流。 |

不新增第三方依赖。`boost-json` 已在 manifest；只移动其 target 使用边界，不更新 baseline。

## 7. 实施任务

```yaml
Task 0:
  type: FIX
  files:
    - shared/include/chathub/contracts/ids.hpp
    - shared/include/chathub/contracts/hello.hpp
    - chat-server/{include,src}/.../hello_codec.*
    - client-qt/{include,src}/.../hello_codec.*
    - tests/hello*_test.cpp
  steps:
    - 修复 M1-7 UUID v4、小写、编码前校验和总正文上限
    - 将服务端 codec 迁入 dispatcher，保留客户端 Qt JSON codec
  done_when:
    - 负例覆盖 UUID v1/大写、空/越界本地编码和大于 64KiB 的 codec 输入

Task 1:
  type: CREATE
  files:
    - chat-server/include/chathub/server/application/hello_handler.hpp
    - chat-server/src/hello_handler.cpp
    - chat-server/include/chathub/server/dispatcher/protocol_dispatcher.hpp
    - chat-server/src/protocol_dispatcher.cpp
  steps:
    - 用纯值对象实现 hello 判定和 capability 交集
    - 定义 protocol_error JSON 与固定错误映射
    - 不创建可注册的 handler 容器
  done_when:
    - handler 不 include Asio/Boost.JSON/SQL，dispatcher 不 include socket

Task 2:
  type: CREATE
  files:
    - chat-server/include/chathub/server/transport/session.hpp
    - chat-server/src/session.cpp
  steps:
    - 构造时接收已握手的 shared_ptr<SslStream> 与连接 identity；start 后 arm hello timer
    - strand 上实现读循环、FrameDecoder 失败映射、状态门、dispatcher 调用、单 timer 与串行写
    - 关闭路径保证 protocol_error 最多一次，写完关闭；bad_magic 直接关闭
  preserve:
    - M1-6 handshake 的 stream 所有权语义
    - FrameDecoder 的失败锁存和不重同步行为
  done_when:
    - Session 不访问 MySQL、令牌、Qt、原始 JSON 或业务 Use Case

Task 3:
  type: TEST
  files:
    - tests/hello_integration_test.cpp
    - tests/session_timeout_test.cpp
    - tests/CMakeLists.txt
  steps:
    - 复用 M1-6 的私有 CA、QSslSocket、acceptor、io 线程守卫和 Qt 事件循环
    - 集成测试在同一次 TCP/TLS 连接上发送 encodeFrame(hello_request, ...)，并解帧验证响应
    - timeout 测试使用真实短 deadline 配置或可注入 clock/duration 值；生产常量仍为 5/20/60 秒
  done_when:
    - 新增两个 CTest target；不使用上次生成的可执行文件
```

## 8. 验收矩阵

| 层级 | 场景 | 必须断言 |
| --- | --- | --- |
| handler | 版本、平台、交集、固定参数、确定时间 | 成功结果仅含 `text_v1` 交集；拒绝不访问外部资源。 |
| Session unit | 状态不允许、重复 hello、decoder 失败、timer 旧代次、队列计数 | 关闭后不再读／写／二次发错；旧 timer 不影响新状态。 |
| 真实 TLS 集成 | 正确 CA/IP + hello | 收到 `hello_response`，秒级时间、连接参数与 capability 对齐；服务端进入 unauthenticated。 |
| 真实 TLS 集成 | 首帧 heartbeat／登录等非 hello | 收到 `invalid_connection_state` protocol_error 后连接关闭。 |
| 真实 TLS 集成 | 5 秒不发 hello | TLS 成功后服务端在约 5 秒关闭；不会进入 unauthenticated。 |
| 真实 TLS 集成 | `client_version="9.9"` | 收到 `unsupported_client_version` 与 `supported_client_version="1.0"` 后关闭。 |
| 未认证超时 | hello 成功后仅发 heartbeat／不发认证 | heartbeat 不延长 60 秒认证截止。 |
| 延后验收 | 认证后 20/60 心跳、真实慢客户端顶满队列 | M2 的认证／业务输出可达后，以真实路径验证；M1-8 不计为通过。 |

预期新增两个 target 后 CTest 为 11 项；最终报告必须分别报告 Passed／Failed／Skipped。若新增 M1-7 修复测试导致数量高于 11，以实际 CTest 列表为准，不为凑数删除必要负例。

## 9. 验证命令

先在活动 CLion 配置 `windows-mingw-debug-local` 重新配置并构建，再运行：

```powershell
cmake --preset windows-mingw-debug-local
cmake --build --preset windows-mingw-debug-local
ctest --test-dir out/build/windows-mingw-debug --output-on-failure
```

还必须人工观察：错误 CA／IP 仍不能降级明文；错误关闭后客户端不自动重连（重连实现属后续客户端能力）。

## 10. 停止条件与文档同步

- hello deadline 后仍能进入 unauthenticated、状态不允许的 type 仍被 handler 处理、旧 timer 触发新状态、或 protocol_error 未写完即被错误路径丢弃：停止并先缩小复现。
- 若要为测试伪造 authenticated、伪造慢客户端堆满业务帧，停止；这说明测试跨入 M2，需要另立能力点或得到明确授权。
- 若实现要求改变 D68/D69/D70/D72/D73/D111/D200 的 wire、状态或验收含义，先更新本 PRP 并由用户确认是否解锁正式设计。
- 实施完成后同步 `docs/周计划/W2-需求与验收.md`、`W2-功能代码对照.md` 和本 PRP 的事实状态；不得把 M2 才能验证的认证后心跳写成 M1-8 已通过。

## 11. 完成清单

- [x] 装配点、协议错误、状态／timer／写队列边界已规划；
- [x] M1-7 修复前置条件与 M2 延后验收已记录；
- [ ] Task 0 合同修复与负例测试；
- [ ] Task 1 handler／无注册表 dispatcher；
- [ ] Task 2 TLS Session；
- [ ] Task 3 真实 TLS→frame→hello 与 timeout 测试；
- [ ] 周文档按真实验证结果同步。
