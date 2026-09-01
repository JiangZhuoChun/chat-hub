# ChatHub 协议字段与状态流向总图

> 状态：持续维护 | 创建：2026-08-13 | 当前范围：W8–W13 已实现的协议、存储与基础设施边界
>
> 用途：任何新增、删除或变更协议字段、消息身份、状态值前，先更新本文件；实现、测试和需求文档在同一次提交中同步更新。本文件用于核对流向，具体校验规则以对应需求文档和代码为准。

---

## 一、使用规则

每次改协议时按下面顺序检查，而不是只搜索字段名：

```text
字段生成者
  → 发出帧 JSON
  → 接收帧 JSON 校验
  → 服务端内存 / IMessageRepository
  → Qt ChatMessage 模型
  → UI 状态或渲染
  → 单元测试 / 人工验收
```

变更清单：

- [ ] 在“字段责任表”中写清生成者、唯一性和用途；
- [ ] 在“帧正文合同”中增删相应字段；
- [ ] 在“状态机”中写清新状态的进入和退出条件；
- [ ] 更新服务端解析、客户端解析、内存结构和 Repository 业务映射；
- [ ] 为正常、缺失字段、错误类型和重复请求补测试；
- [ ] 更新对应 W8/W9/W10/W11 需求文档和学习笔记。

---

## 二、核心字段责任表

| 字段                      | 生成者                        | 唯一范围       | 主要用途                                        | 不应承担的职责            |
|-------------------------|----------------------------|------------|---------------------------------------------|--------------------|
| `local_id`              | A 的 Qt 客户端                 | 同一发送者的发送意图 | 本地气泡定位、重试幂等键、`chat_ack` 与错误帧关联              | B 的回执、历史去重、跨用户全局身份 |
| `message_id`            | ChatServer 生成候选值，首次 Repository 成功提交时确定 | 全局         | 持久消息主键、B 的 `delivery_receipt`、历史去重、稳定排序平手决胜 | A 侧发送前的临时气泡定位      |
| `from`                  | ChatServer 从认证 Session 推导  | 一条消息       | 真实发送者、B 的会话归属                               | 客户端自报身份            |
| `to`                    | A 提交，ChatServer 校验在线状态     | 一条消息       | 接收者、持久记录中的 `recipient` 字段                    | 回执发送者身份            |
| `content`               | A 的 Qt 客户端                 | 一条消息       | 聊天正文，最多 1024 字节                             | UI 状态或服务端排序依据      |
| `send_at`               | A 的 Qt 客户端                 | 一条消息       | UI 展示时间，ISO UTC 字符串                         | 服务端可信历史排序          |
| `server_received_at_ms` | ChatServer 首次入库前           | 一条持久记录     | 历史与实时合并排序；与 `message_id` 组成稳定游标             | UI 发送时间显示或客户端自行生成  |
| `status`                | 协议发送者                      | 状态通知帧      | `accepted`、`delivered` 的状态语义                | 消息的持久化身份           |
| `failure_reason`        | Qt 客户端从错误帧转换               | 本地模型       | Failed 气泡的显示原因                              | 网络协议字段             |
| `sender_session_id`     | ChatServer                 | 当前进程       | 待送达时找到 A 的当前连接                              | 持久消息身份或跨重启恢复       |
| `recipient_username`    | ChatServer                 | 当前进程       | 验证回执者确为 B                                   | 从回执正文取得身份          |

### `local_id` 与 `message_id` 的分工

```mermaid
flowchart LR
    A["A Qt 客户端\n生成 local_id"] --> C["chat 请求\nlocal_id + to + content + send_at"]
    C --> S["ChatServer\n认证、校验、生成候选 message_id"]
    S --> REPO["IMessageRepository\nstoreMessage 持久化"]
    REPO --> M["StoreOutcome\n确定持久 message_id"]
    M --> ACK["chat_ack 给 A\nlocal_id + message_id + server_received_at_ms + accepted"]
    M --> FWD["chat 给 B\nmessage_id + local_id + from + to + content + send_at + server_received_at_ms"]
    FWD --> RCPT["B 回执\n仅携带 message_id"]
    RCPT --> P["PendingDelivery[message_id]\n取回 sender_local_id"]
    P --> D["delivered 给 A\nlocal_id + delivered"]
```

结论：`message_id` 负责跨网络、跨用户、可持久的“这是哪条消息”；`local_id` 负责 A 本地“要更新哪个既有气泡”。

---

## 三、帧正文合同

`type`、帧方向和字段必须一起理解。客户端发送的 `chat` 没有 `message_id`，因为它尚未入库；服务端转发给 B 后才有。

| type | 名称 / 方向                          | 当前正文                                                                          | 关键校验与副作用                                                                      |
|-----:|----------------------------------|-------------------------------------------------------------------------------|-------------------------------------------------------------------------------|
|    1 | `chat`，A → Server                | `{ to, content, local_id, send_at }`                                          | Server 从认证会话取得真实 `from`；校验字段、认证、接收者在线，并通过 `IMessageRepository` 持久化              |
|    1 | `chat`，Server → B                | `{ message_id, local_id, from, to, content, send_at, server_received_at_ms }` | B 全部字符串字段必须非空，`send_at` 必须可解析，服务端时间必须是非负整毫秒；写入 `ChatMessage` 后再回执             |
|    4 | `error`，Server → Client          | `{ scope, code, message, local_id? }`                                         | 有 `local_id`：A 将指定气泡置为 `Failed`；无 `local_id`：作为普通服务端错误                        |
|    5 | `auth`，Client → Server           | JWT 原始文本                                                                      | 未认证 Session 只接受此帧                                                             |
|    5 | `auth`，Server → Client           | `{ ok: true }`                                                                | 客户端进入已认证状态                                                                    |
|    6 | `chat_ack`，Server → A            | `{ local_id, message_id, status: "accepted", server_received_at_ms }`         | A 用 `local_id` 找到既有气泡，写入 `message_id` 和非负整毫秒服务端时间，状态改为 `Accepted` 后按服务端顺序整理会话 |
|    7 | `delivery_receipt`，B → Server    | `{ message_id }`                                                              | Server 校验 B 的认证身份等于 `PendingDelivery::recipient_username`                     |
|    7 | `delivery_receipt`，Server → A    | `{ local_id, status: "delivered" }`                                           | A 仅改既有气泡状态，不新建气泡、不置顶                                                          |
|    8 | `online_users`，Server → Client   | `{ users: ["alice", "bob"] }`                                                 | 客户端严格校验字符串数组、去重后整体替换在线列表                                                      |
|    9 | `history_query`，Client → Server  | `{ request_id, limit, before? }`                                              | 已认证后发起；身份只能从认证 Session 推导，首屏上限 50 条                                           |
|   10 | `history_result`，Server → Client | `{ request_id, messages, is_last_chunk, has_more, next_cursor }`              | 已实现；每条历史记录含 `message_id`，按实际编码字节分块，客户端仅在最终块合并                                 |

其中 W9 历史单条记录的当前字段：

```json
{
  "message_id": "server-uuid",
  "local_id": "sender-client-local-id",
  "from": "alice",
  "to": "bob",
  "content": "hello",
  "send_at": "2026-08-12T10:00:00.000Z",
  "server_received_at_ms": 1786490400000
}
```

---

## 四、服务端数据流与保存位置

```mermaid
flowchart TD
    C["A → Server: chat\nlocal_id, to, content, send_at"]
    V["Server 校验\n帧 / 正文 / 认证 / 当前会话 / B 在线"]
    RP["IMessageRepository.storeMessage\n统一写入与幂等合同"]
    SQ["SQLite messages\n默认后端"]
    MY["MySQL messages\n显式选择的后端"]
    O["StoreOutcome\n持久记录与结果状态"]
    P["内存 PendingDeliveryMap\nmessage_id → sender_username, sender_session_id, sender_local_id, recipient_username"]
    B["转发给 B\n含 message_id + server_received_at_ms"]
    A["chat_ack 给 A\nlocal_id + message_id + server_received_at_ms"]
    R["B → Server: delivery_receipt\nmessage_id"]
    D["认证身份校验\n回执者 == recipient_username"]
    N["通知 A\nlocal_id + delivered"]

    C --> V --> RP
    RP --> SQ --> O
    RP --> MY --> O
    O --> P --> B
    P --> A
    B --> R --> D --> N
    D --> X["删除 PendingDelivery[message_id]"]
```

| 位置                   | 保存什么                                                           | 生命周期                 | 重启后                          |
|----------------------|----------------------------------------------------------------|----------------------|------------------------------|
| A 的 `ChatMessage`    | `local_id`、收到 ack 后的 `message_id`、`server_received_at_ms`、展示状态 | 客户端内存；认证后合并历史与实时消息   | 重连后由显式历史查询恢复，不伪造 `Delivered` |
| Repository `messages` | 一条消息的完整持久业务数据；具体保存在 SQLite 或 MySQL                                | 所选数据库后端              | 保留，可供历史查询                    |
| `PendingDeliveryMap` | 回执归属与 A 侧气泡定位信息，不保存正文                                          | Server 进程内，直至回执或相关断线 | 清空，不能恢复 `Delivered`          |

存储后端由 ChatServer 启动配置选择，不属于网络协议。客户端不能在帧中指定 SQLite/MySQL，线上帧也不携带后端名称；两种实现都必须返回相同的
`StoreOutcome`、历史记录和错误语义。

---

## 四点五、W13 基础设施与认证 API 边界

Docker Compose 只提供本机 MySQL 与 Redis；它没有容器化 Auth Service、ChatServer 或 Qt Client，也没有改变 TCP 帧、HTTP API 或任一状态的所有者。

```mermaid
flowchart LR
    Q["Qt Client"] -->|"HTTP 登录"| A["Auth Service\n主机 :3000"]
    Q -->|"TCP auth/chat"| S["ChatServer\n主机 :9000"]
    S -->|"HTTP /internal/auth/introspect"| A
    A -->|"限流、撤销 marker，TTL"| R["Compose Redis\n127.0.0.1:6380"]
    S -->|"仅显式 MySQL 后端"| M["Compose MySQL\n127.0.0.1:3307"]
    S -->|"默认后端"| D["SQLite"]
```

| 组件 / API | 输入与输出 | 拥有的数据 | W13 不能改变的边界 |
|---|---|---|---|
| Auth Service | 登录/注销、`/me`、`/internal/auth/introspect`；输出 JWT 或认证结论 | 用户 SQLite、密码哈希、JWT 签发；Redis 登录限流与撤销 marker | 不路由聊天，不保存聊天正文；真实内部服务密钥和 JWT 不进文档/日志 |
| Redis | Auth Service 的受限 key 读写；撤销/限流 key 带 TTL | 临时认证状态 | 不是真实消息库，不保存密码、完整 JWT 或用户资料；ChatServer 不直接连接 Redis |
| ChatServer | TCP `auth` 携带 JWT；向 Auth introspection 取得可信用户名；TCP 聊天帧输出 ACK/转发/送达或错误 | Session、在线映射、`PendingDeliveryMap` | 不信任客户端自报发送者；Compose 不会替它完成认证；新认证依赖不可用时必须 fail-closed |
| MySQL / SQLite | `IMessageRepository` 的相同业务合同 | 已持久化聊天消息与历史排序字段 | MySQL 仅在启动时显式选择；Compose MySQL 不会把默认 SQLite 自动迁移或替换 |

认证调用顺序固定为：Client 提交 TCP `auth` → ChatServer 带内部服务凭证调用 Auth introspection → Auth 校验 JWT 与 Redis 撤销 marker →
ChatServer 只在得到可信用户名后建立 Session。Auth 不可用时，新连接返回 `scope=auth` / `code=authentication_dependency_unavailable`；当前合同不要求主动重验或踢除已认证 Session。

消息调用顺序仍是：可信 Session 的 `chat` → Repository 成功持久化 → `chat_ack` → 接收方转发 → `delivery_receipt`。若运行期 MySQL 写入失败，发送方收到
`scope=chat` / `code=database_write_failed`，且不得伪造 ACK 或转发。

---

## 五、发送方 A 的状态机

```mermaid
stateDiagram-v2
    [*] --> Sending: 点击发送，先创建本地 ChatMessage
    Sending --> Failed: 未认证 / 写帧失败 / Server error(local_id)
    Sending --> Accepted: chat_ack(local_id, message_id, accepted)
    Failed --> Sending: 用户重试，保留同一个 local_id
    Accepted --> Delivered: delivery_receipt(local_id, delivered)
    Delivered --> [*]

    note right of Accepted
        Repository 已成功提交且 Server 已接受。
        不等于 B 已处理。
    end note
    note right of Delivered
        B 已写入本地模型并完成本轮 UI 处理。
        不等于“已读”。
    end note
```

状态变更约束：

| 状态          | 进入条件                     | 可改动的数据                                               | 禁止行为              |
|-------------|--------------------------|------------------------------------------------------|-------------------|
| `Sending`   | 本地创建消息或对失败消息重试           | `status`、`failure_reason`                            | 仅恢复 Sending 时置顶会话 |
| `Failed`    | 本地失败或带 `local_id` 的服务端错误 | `status`、`failure_reason`                            | 不创建第二个气泡          |
| `Accepted`  | 收到合法 `chat_ack`          | `message_id`、`server_received_at_ms`、`status`、清空失败原因 | 不代表 B 已收到         |
| `Delivered` | 收到合法服务端最终通知              | `status`、清空失败原因                                      | 不新建气泡、不增加未读、不置顶   |
| `Received`  | B 收到 Server 转发的 chat     | 完整消息字段                                               | 不能显示为本人“已送达”      |

---

## 六、接收方 B 的本地处理顺序

```mermaid
sequenceDiagram
    participant S as ChatServer
    participant B as B 的 ChatClient
    participant U as B 的 MainWindow
    participant A as A 的 ChatClient

    S->>B: chat(message_id, local_id, from, to, content, send_at)
    B->>B: 严格校验 JSON 字段类型与时间
    B->>U: chatMessageReceived(ChatMessage)
    U->>U: 写入 m_conversations
    U->>U: 更新会话、未读或当前气泡
    U->>B: sendDeliveryReceipt(message_id)
    B->>S: delivery_receipt(message_id)
    S->>A: delivery_receipt(sender_local_id, delivered)
```

回执的含义是“B 已完成本阶段规定的本地模型和 UI 处理”。若 B 的回执发送失败，B 已收到的消息不回滚，A 维持 `Accepted`。

---

## 七、异常与重复路径

| 场景                    | 使用的关联字段                                    | 预期结果                                                                          |
|-----------------------|--------------------------------------------|-------------------------------------------------------------------------------|
| A 用同一请求重试             | `local_id` + 认证发送者                         | Repository 返回既有 `message_id`；不重复写入、不重复转发                                      |
| A 复用 `local_id` 但正文不同 | `local_id` + 认证发送者                         | `idempotency_conflict`，原持久记录不变                                                |
| B 回执未知 `message_id`   | `message_id`                               | Server 只回复回执错误，不通知 A                                                          |
| C 猜中某条 `message_id`   | `message_id` + C 的认证身份                     | `recipient_username` 不匹配，拒绝，不通知 A                                             |
| B 重复回执                | `message_id`                               | 第一条回执后待送达记录已删除；第二条被拒绝                                                         |
| A 或真正离线的 B 断线         | `sender_session_id` / `recipient_username` | 删除相关 `PendingDelivery`；A 不被伪造为 `Delivered`                                    |
| ChatServer 重启         | 内存表清空                                      | 所选 Repository 后端的历史仍在；`Delivered` 不恢复                                        |
| 未认证连接达到截止             | 无可信身份                                      | 返回 `scope=auth` / `code=authentication_timeout` 后关闭，不进入在线映射                   |
| Auth introspection 不可用 | 新 TCP `auth` 需要可信身份结论                           | 返回 `scope=auth` / `code=authentication_dependency_unavailable`；新连接不放行，旧 Session 不主动重验 |
| 运行期 MySQL 写入失败 | Repository 在持久化聊天时失败                              | 返回 `scope=chat` / `code=database_write_failed`；不得发送 `chat_ack` 或转发给接收者 |
| 第 89 名用户认证            | 完整在线快照容量                                   | 返回 `scope=online_users` / `code=online_snapshot_capacity_exceeded` 后关闭，既有映射不变 |

---

## 八、关联文档

- [W9 需求文档：SQLite 持久化](W9需求文档-SQLite持久化.md)
- [W11 需求文档：MySQL 集成与存储抽象](W11需求文档-MySQL集成与存储抽象.md)
- [W12 需求文档：Redis 临时状态与认证安全](W12需求文档-Redis临时状态与认证安全.md)
- [W13 需求文档：工程交付与可重复验证](W13需求文档-工程交付与可重复验证.md)
- [W8-9 最终送达回执需求文档](W8-9最终送达回执需求文档.md)
