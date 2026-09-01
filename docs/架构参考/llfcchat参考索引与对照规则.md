# llfcchat 参考索引与对照规则

> 参考仓库：`C:\Users\Administrator\Desktop\llfcchat-master`。本文是 ChatHub 的**对照索引**
> ，不是复制清单；参考仓库未在本轮构建或运行，以下结论仅来自目录、文档和源码的静态阅读。

## 1. 使用顺序

每个新能力严格按下面顺序学习：

1. 先由自己画出 ChatHub 的职责、数据流、失败路径和验收；
2. 自己完成最小设计或实现尝试；
3. 再只打开本表指定的一个文件、一个函数附近的 10–30 行；
4. 用代码块记录“它解决什么问题、ChatHub 为什么相同或不同”；
5. 自己补一条测试或故障复盘。不得整类复制、不得把参考仓库的单例、命名或工程结构直接搬入。

教学时引用格式：`[llfc:相对路径:行号]`。代码块只展示当前问题所需的局部，随后必须标出 ChatHub 的自主方案和取舍。

## 2. 必须保留的 ChatHub 边界

- 保留 CMake、CTest、`asio::strand`、现有 `ChatMessage` 模型和客户端 Model/View 边界；不改成 llfcchat 的 `.pro` / Visual
  Studio 多工程组织。
- 不复制全局 `Singleton`、全局逻辑队列或多 ChatServer/StatusServer/GateServer 拆分；ChatHub 当前目标仍是可验证的单机系统。
- 不手改 `message.pb.*`、`message.grpc.pb.*` 等生成文件；Protobuf/gRPC、文件、音视频、跨服路由只留在后置 spike。
- MySQL 和 Redis 必须先有业务责任与测试。客户端不能直连它们；Redis 不保存聊天正文。

## 3. 分阶段参考索引

| ChatHub 阶段      | llfcchat 参考位置                                                                                                                            | 借鉴内容                            | ChatHub 的不同取舍                                                       |
|-----------------|------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------|---------------------------------------------------------------------|
| W9 历史/持久化       | `server/ChatServer/LogicSystem.h` 的 `GetUserThreads`、`CreatePrivateChat`、`LoadChatMsg`                                                   | 会话线程、创建私聊、分页加载三类职责              | 先设计 `message_id`、SQLite schema 和 Repository，不把数据访问塞进全局 LogicSystem。 |
| W10 Session/可靠性 | `server/ChatServer/CSession.h` 的读头、读体、发送队列、心跳声明                                                                                          | 分帧、写队列、保活是 Session 职责           | ChatHub 已有 `strand`；不同时叠加互斥锁作为并发模型。                                 |
| W11 MySQL       | `server/GateServer/MysqlDao.h/.cpp`                                                                                                      | 预编译语句、事务、连接失效是需要显式处理的边界         | 先定义 `MessageRepository`，SQLite 与 MySQL 实现遵守同一业务测试；不默认引入全局 DAO 管理器。  |
| W12 Redis       | `server/GateServer/RedisMgr.h`                                                                                                           | 连接失败、TTL 和资源归还是 Redis 客户端要考虑的问题 | 首个业务场景仅为 Auth 登录限流；不为了“用 Redis”搬入完整自建连接池。                           |
| W14 联系人与好友闭环    | `LogicSystem.h` 的 `SearchInfo`、`AddFriendApply`、`AuthFriendApply`、`GetFriendList`；客户端 `applyfriend.h`、`contactuserlist.h`、`chatdialog.h` | 搜索→申请→接受/拒绝→联系人→进入私聊的业务状态       | 自己设计关系表、权限校验、HTTP/TCP 分工和 Qt 模型；不复制 UI 类名或直接沿用其协议。                  |

## 4. 局部代码参考示例

llfcchat 将聊天、好友申请、联系人和历史读取注册为不同的逻辑处理职责：

```cpp
// [llfc:server/ChatServer/LogicSystem.h:29-49]
void SearchInfo(...);
void AddFriendApply(...);
void AuthFriendApply(...);
void DealChatTextMsg(...);
bool GetFriendList(...);
bool GetUserThreads(...);
void CreatePrivateChat(...);
void LoadChatMsg(...);
```

这段代码只用于观察“功能边界会自然长成多个用例”。ChatHub 将在 W14 自己命名接口、自己定义请求体和测试；其中好友关系必须成为发送私聊前的权限条件，而不是
UI 里的一个列表。

llfcchat 的 Session 也明确把读头、读体和写队列放在网络连接对象中：

```cpp
// [llfc:server/ChatServer/CSession.h:34-45]
void Start();
void Send(std::string msg, short msgid);
void AsyncReadBody(int length);
void AsyncReadHead(int total_len);
bool IsHeartbeatExpired(std::time_t& now);
void UpdateHeartbeat();
```

这说明 Session 应只管理连接生命周期和帧收发；业务路由、好友权限和持久化不应被塞进 `Session`。ChatHub 继续采用现有 `strand`
串行化同一会话/服务器共享状态。

## 5. 何时停止参考

若自己的设计已经能解释数据归属、失败行为和测试，就停止继续阅读 llfcchat；它不是“标准答案”。卡住超过两小时才带着错误、已试方案和本表的一个定位向教学会话提问。
