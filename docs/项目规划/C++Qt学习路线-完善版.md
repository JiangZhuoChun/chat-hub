# C++ / Qt 全栈聊天软件学习路线（完善版）

> **制定日期**：2026-09-01  
> **项目定位**：Windows 局域网 C++20／Qt 6 桌面单聊软件，作为简历主项目边学边做。  
> **产品基线**：v1.2，共 42 条首版需求；详细设计 D01—D231 已确认。  
> **当前状态**：仅规划，尚未复制旧工程、实现新架构、构建或运行验收。  
> **当前能力点**：M0-1——安全复制旧工程并建立迁移清单；只有用户说“下一步”后才开始教学与实施。  
> **学习投入**：每天 8 小时，每周 7 天按 56 小时排期；建议用 16 个学习周完成 M0—M7，未通过门禁时整体顺延。  
> **AI 协作**：AI 全程负责概念教学、任务拆分、代码审查、调试、自动化测试设计与测试代码；用户默认编写生产代码并完成解释和人工验收。  
> **路线边界**：本路线是本地学习进度事实源，不进入项目 Git；产品合同以 `./首版产品方案.md` 和 `./详细设计与技术选型.md` 为准。

这条路线不要求先把所有技术学完再开发，而是围绕 M0—M7 逐步交付纵向能力。每轮只学习和实现一个可验收能力点；实现、自动验证、人工验收和真正掌握分别记录。AI 可以直接补充测试代码，但用户必须理解关键断言、测试隔离和失败原因；AI 生成或运行通过不能代替掌握证据。旧 ChatHub 只提供可迁移的代码经验，不自动证明新项目已完成或已经掌握。

---

## 进度总览

```text
产品与架构设计  ████████████ 100% [v1.2／42 条需求／D01—D231 已锁定]
M0 构建基线     ░░░░░░░░░░░░   0% [下一能力点：M0-1 安全复制]
M1 协议与 TLS   ░░░░░░░░░░░░   0%
M2 数据与秘密   ░░░░░░░░░░░░   0%
M3 认证闭环     ░░░░░░░░░░░░   0%
M4 好友闭环     ░░░░░░░░░░░░   0%
M5 文字单聊     ░░░░░░░░░░░░   0%
M6 缓存与 UI    ░░░░░░░░░░░░   0%
M7 Windows 交付 ░░░░░░░░░░░░   0%
首版后扩展       锁定         [文件传输→语音消息→语音转文字，均待另行设计]
```

> **计划周期**：W0—W15，共 16 个全职学习周、约 896 小时名义投入。该数字用于分配容量，不是完成声明；返工、环境故障和理解检查都会使阶段顺延。

| 维度 | 当前证据 | 首版目标 |
| --- | --- | --- |
| 产品与架构 | ✅ 42 条需求、D01—D231 已写入正式文档 | 实现与测试逐项可追踪 |
| 现代 C++ | 🟡 旧项目有实践，新项目尚无强类型／分层证据 | 能设计、修改和迁移 C++20 核心模型 |
| Qt 6 Widgets | 🟡 旧项目有功能界面，新项目正式 UI 未实现 | Model/View、delegate、线程和托盘可解释 |
| Asio／TLS | 🟡 旧项目有 Asio TCP，当前 TLS 合同未实现 | 能解释 Session、分帧、strand、TLS 和重连 |
| MySQL／SQLite | 🟡 旧项目有存储实践，新模型未迁移 | 事务、仓储、迁移、加密缓存有真实验证 |
| 安全 | ❌ 新项目 Argon2id、DPAPI、CredMan、私有 CA 未实现 | 能说明每种安全机制保护的边界 |
| 工程交付 | ❌ 新根目录尚非 Git 仓库，也没有可复现构建 | Presets、测试、安装包和干净机验收完整 |
| 项目表达 | ❌ 只有设计合同，没有新项目运行证据 | 可演示、可复盘、可在简历中如实表述 |

> **状态标记**：`✅ 已掌握` · `🟡 学习中` · `❌ 未学或无新项目证据` · `🔴 卡住` · `🔒 首版后`。
>
> **更新规则**：只有新鲜源码、构建／测试、人工验收和解释证据支持时才更新。代码存在或构建通过不能单独标记掌握。

---

## 1. 当前基础与证据校准

### 1.1 可以复用的经验

- 旧项目已经接触 Qt Widgets、Qt Model/View、QTcpSocket、standalone Asio、TCP 分帧、消息状态、SQLite、MySQL、认证和测试。
- 旧代码可以作为迁移蓝图，帮助识别已有类、依赖和可复用测试思路。
- 已完成的产品访谈和 D01—D231 设计减少了实现阶段的猜测。

### 1.2 必须重新证明的能力

- 新项目固定 C++20、Qt 6.11.1 MinGW 64-bit、manifest vcpkg 和模块化 CMake targets；旧工程能编译不等于新基线可复现。
- 新协议使用 8 字节帧头、TLS、hello capability、稳定 type 注册表和类型化 DTO；旧协议不得直接视为兼容。
- 认证统一进入 C++ 服务端，旧 Node.js auth-service 最终退出运行链路。
- 服务端使用 MySQL 8.4.11，客户端使用加密 SQLite 缓存；两者的数据所有权和失败边界不同。
- 正式 UI、Windows 服务、私有 CA、安装包、100 在线账号和每秒 50 条消息都没有新项目运行证据。

### 1.3 学习与交付的四种状态

| 状态 | 判定 |
| --- | --- |
| 设计已确认 | 需求、边界、数据流和验收已写清，尚不代表有代码 |
| 代码已实现 | 生产代码存在，但可能未经过本轮有效验证 |
| 运行验收通过 | 自动测试、人工路径或性能测试有新鲜证据 |
| 已掌握 | 用户达到“能改、能讲、能迁移”，通常对应 L4／L5 |

---

## 2. 相对旧路线的关键调整

| # | 旧项目或常见做法 | 当前路线调整 | 原因 |
| --- | --- | --- | --- |
| 1 | Node.js 认证服务与 C++ ChatServer 分离 | 认证、好友和消息统一进入一个 C++ 模块化单体 | 符合已确认的首版部署和 C++ 学习目标 |
| 2 | 先沿旧代码继续堆功能 | 先做 M0 可复现构建，再用 Branch by Abstraction 渐进替换 | 保护旧仓库，避免一次性重写失去可运行基线 |
| 3 | UI、协议和数据模型容易互相引用 | 固定 UI→presentation→application/domain→ports 的依赖方向 | 让文件／语音扩展不迫使主窗口和 Session 重写 |
| 4 | JSON 结构可能贯穿多层 | Wire DTO、Command／Query、Domain、Record、View Item 分离 | 分开协议、业务、数据库和展示约束 |
| 5 | TCP 可用后再考虑安全 | M1 直接建立 TLS、私有 CA、hello 和超时状态机 | 首版不允许明文降级，安全是基础合同 |
| 6 | 消息 ID 同时承担排序和重试 | 分开 `message_id`、`local_id`、`delivery_seq`、`conversation_seq` | 各自解决身份、幂等、补收和会话顺序 |
| 7 | 先实现界面再补后台一致性 | 先命令行／测试跑通协议、事务和错误路径，最后接正式 Qt UI | 减少 UI 掩盖网络和持久化问题 |
| 8 | 为未来文件和语音预建空类 | 首版只实现文字和 `UnknownPayload` 扩展边界 | 不为未确认行为提前造代码 |
| 9 | 一周进度按时间宣布完成 | 任一门禁未过就继续当前周，后续整体顺延 | 保证学习和项目证据真实 |

---

## 3. 目标架构：首版 v1.2

```text
Qt Widgets View
      ↓ intent / state snapshot
Presentation Model + AppController
      ↓ Command / Query
Client Application + Domain + Ports
      ↙                      ↘
Qt TLS Network Adapter       Encrypted SQLite / Windows Adapters
             ║ TLS control connection
             ║ 8-byte frame + compact UTF-8 JSON
Asio TLS Session → ProtocolDispatcher → Command Handler / Use Case
                                      ↓
                             Domain + Repository Ports
                                      ↓
                    MySQL / Argon2 / Windows / Logging Adapters
```

### 首版必须实现

- Windows 局域网部署、私有 CA TLS、注册／登录／密码重置、记住密码和单点登录。
- 昵称、默认头像、精确账号查找、好友申请、接受／拒绝／撤回、删除和重加。
- 文字与 Emoji 单聊、幂等重试、历史、未读、清空个人会话、断线补收。
- 加密 SQLite 缓存、同机多 profile、托盘、正式三栏 Qt Widgets UI。
- 管理员关闭注册、停用／恢复账号、账号容量和低磁盘保护。
- Windows 服务、离线安装、真实 MySQL／TLS／双客户端 E2E、性能和 UI QA。

### 首版明确不做

- 群聊、图片、文件传输、语音消息、语音转文字、音视频通话。
- 端到端加密、服务端正文应用层加密、Web 管理后台、账号永久删除和数据导出。
- Redis、微服务、DLL 插件、自动更新、遥测、自动日志上传和备份系统。

---

## 4. W0—W15 全职主线计划

> 本计划按每天 8 小时、每周 56 小时编排，共 16 个学习周。每周可以包含多个按顺序执行的微能力，但每次对话和每次代码变更仍只推进一个能力点；用户说“下一步”后才进入下一项。AI 能缩短资料查找、测试编写和排错时间，不能跳过理解、真实依赖验证或人工体验。

### M0／W0 · 安全迁移与可复现构建

| 周次 | 顺序能力点 | 关键概念／API | 本周交付 | 状态 |
| --- | --- | --- | --- | --- |
| W0 | M0-1 安全复制 → M0-2 基线构建 → M0-3 Presets／vcpkg → M0-4 Git 范围 | 文件清单、排除规则、哈希／diff、CMake target、Ninja、CTest、manifest vcpkg、Branch by Abstraction | 不污染旧仓库的新基线，Debug／Release 可复现构建，最小 smoke test | ❌ 下一步 |

**阶段验收（不通过不进入 M1）：**

- [ ] 原 `D:\CppLearn\chathub` 未被修改，新目录不含数据库、秘密、证书私钥、日志、缓存和构建产物。
- [ ] 固定 Qt 6.11.1 MinGW 64-bit、C++20、Ninja、CMake 最低版本、vcpkg commit、`x64-mingw-dynamic` 和依赖版本。
- [ ] 从空构建目录可配置、构建、运行测试和启动旧基线；本机专属路径只进入 User Preset。
- [ ] 新仓库初始化后只纳入允许文件；本路线和唯一学习笔记不进入 Git。
- [ ] 用户能解释 Preset、target、toolchain、triplet、构建目录和依赖锁定的职责。

**简历证据包**：一条干净构建命令、依赖版本快照、构建／测试输出和迁移边界说明。

### M1／W1—W2 · 公共合同、分帧、TLS 与 hello

| 周次 | 顺序能力点 | 关键概念／API | 本周交付 | 状态 |
| --- | --- | --- | --- | --- |
| W1 | M1-1 CMake 分层 → M1-2 强类型 ID／Outcome → M1-3 协议描述 → M1-4 安全 FrameDecoder | static library、PUBLIC／PRIVATE、`enum class`、值对象、`std::variant`、字节序、UTF-8、64 KiB 上限 | shared contracts、ProtocolDescriptor、双端 codec、分帧与架构测试 | ❌ |
| W2 | M1-5 私有 CA → M1-6 TLS 双端 → M1-7 hello／capability → M1-8 Session 超时／心跳 | `QSslSocket`、`asio::ssl::stream`、OpenSSL、SAN、verify peer、strand、steady timer、有界写队列 | TLS 控制连接、状态机、hello、心跳和错误合同 | ❌ |

**阶段验收（D01—D15、D55—D74、D193—D200、D208—D221）：**

- [ ] domain／application target 不链接 Qt、Asio、MySQL、OpenSSL 或 Win32 API。
- [ ] 半包、粘包、空正文、超长正文、非法 UTF-8、未知 type、版本不兼容和慢客户端均有自动测试。
- [ ] 证书链或服务器 IP 校验失败时不发送业务数据，也不降级明文。
- [ ] TCP、TLS、hello 各自 5 秒超时；Session 只拥有连接、帧、超时、写队列和认证后的连接身份。
- [ ] 用户能从 wire bytes 讲到 typed command，并独立修改一个字段边界和对应 codec。

**简历证据包**：协议帧图、TLS 握手失败演示、分帧故障矩阵和架构依赖检查结果。

### M2／W3—W4 · MySQL、配置、秘密与事务基础

| 周次 | 顺序能力点 | 关键概念／API | 本周交付 | 状态 |
| --- | --- | --- | --- | --- |
| W3 | M2-1 配置 schema → M2-2 DPAPI／ACL → M2-3 MySQL RAII → M2-4 连接池／线程所有权 | JSON 配置、composition root、`CryptProtectData`、DACL、MariaDB Connector/C、prepared statement | 启动校验、秘密初始化、RAII 连接和数据库任务池 | ❌ |
| W4 | M2-5 版本迁移 → M2-6 领域仓储 → M2-7 TransactionRunner → M2-8 PostCommitEvent／故障恢复 | DDL、索引、事务隔离、deadlock、typed Outcome、repository contract | schema V1、迁移 CLI、类型化仓储、事务 runner 和真实 MySQL 合同测试 | ❌ |

**阶段验收（D16—D36、D111—D128、D199—D212）：**

- [ ] 正式 MySQL 只通过 Windows shared memory 访问，业务层不出现 `MYSQL*` 或 SQL。
- [ ] 正常启动只检查 schema；显式迁移失败时不开始监听，重复执行具有确定结果。
- [ ] 数据库线程各自拥有连接，事务不跨线程或连接；网络线程不执行同步 SQL。
- [ ] 只有事务提交后才产生成功结果和 post-commit event；提交结果不明不自动重放写操作。
- [ ] 配置、数据库密码、日志和诊断包通过秘密扫描与 ACL 检查。

**简历证据包**：schema／索引图、事务时序、真实 MySQL 合同测试和写入故障隔离演示。

### M3／W5—W6 · 注册、登录、会话与密码重置

| 周次 | 顺序能力点 | 关键概念／API | 本周交付 | 状态 |
| --- | --- | --- | --- | --- |
| W5 | M3-1 输入值对象 → M3-2 Argon2 工作池 → M3-3 两阶段注册 → M3-4 terms／注册限流 | libsodium、Argon2id、CSPRNG、Base32、dummy hash、有界队列、prepare／finalize | 验证器、密码基准、pending registration 和一次性重置码流程 | ❌ |
| W6 | M3-5 登录／token → M3-6 单点会话／恢复 → M3-7 重置／记住密码 → M3-8 admin／低磁盘 | BLAKE2b token digest、`auth_version`、Credential Manager、DPAPI、限流、session termination | 登录、恢复、退出、重置、注册开关、停用／恢复账号 | ❌ |

**阶段验收（AUTH-01—AUTH-10、ADMIN-01、D37—D46、D78—D91、D186—D190）：**

- [ ] 数据库和日志中没有密码、原始重置码、原始 token 或私钥。
- [ ] prepare 未 finalize 的账号不能登录；重置码只展示一次，账号创建后仍须手动登录。
- [ ] 不存在账号也执行 dummy Argon2；登录和重置的账号／IP 限流语义一致。
- [ ] 新登录事务提交后才挤掉旧登录；密码重置和账号停用撤销全部会话。
- [ ] 关闭注册不影响已有账号登录；达到 1000 账号或低磁盘时行为符合产品合同。
- [ ] 用户能解释 TLS、Argon2id、token 摘要、DPAPI 和 Credential Manager 各自保护的边界。

**简历证据包**：认证状态图、Argon2 P95、单点登录／停用演示和安全边界说明。

### M4／W7 · 资料、好友申请与联系人闭环

| 周次 | 顺序能力点 | 关键概念／API | 本周交付 | 状态 |
| --- | --- | --- | --- | --- |
| W7 | M4-1 资料／搜索 → M4-2 申请状态机 → M4-3 关系版本 → M4-4 联系人投影／系统事件 | Unicode 字素、Query、cursor、pending、幂等、冷却、`friendship_version`、projection | 资料、精确搜索、申请、接受／拒绝／撤回、删除／重加和联系人列表 | ❌ |

**阶段验收（USER-01—USER-02、FRIEND-01—FRIEND-05、D47—D50、D88—D101）：**

- [ ] 资料更新只接受更高版本，账号名不可修改，12 个头像 ID 稳定。
- [ ] 同向重复申请幂等，反向 pending 自动建好友；达到好友或申请上限时保持一致。
- [ ] 删除好友一次性移除双方联系人，但保留双方历史；重加不制造第二段虚假历史。
- [ ] 非好友发送在消息保存事务内被拒绝，不能靠客户端按钮状态充当权限。
- [ ] 用户能画出好友申请和 FriendshipPair 的状态转换及并发冲突处理。

**简历证据包**：好友状态机、并发申请测试、双向删除／重加 E2E 和联系人投影说明。

### M5／W8—W10 · 文字消息、历史、未读与补收

| 周次 | 顺序能力点 | 关键概念／API | 本周交付 | 状态 |
| --- | --- | --- | --- | --- |
| W8 | M5-1 消息四层模型 → M5-2 payload variant → M5-3 本地幂等 → M5-4 提交后确认／推送 | MessageEnvelope、UUID、mapper、唯一约束、post-commit event、写队列 | 文字发送、确认、实时推送、迟到结果和手动重试 | ❌ |
| W9 | M5-5 delivery 补收 → M5-6 历史 cursor → M5-7 read／clear watermark → M5-8 会话排序／未读 | 连续水位、重复下发、backoff、tombstone、conversation seq、projection | 恢复会话、补收、历史分页、未读、清空和会话列表 | ❌ |
| W10 | M5-9 故障隔离 → M5-10 背压／限流 → M5-11 100 在线 → M5-12 50 条／秒性能基线 | bounded queue、fault injection、load generator、P50／P95、数据守恒 | 真实 MySQL／TLS 的可靠性、容量和性能阶段报告 | ❌ |

**阶段验收（MSG-01—MSG-11、CONN-01—CONN-03、D10—D32、D51—D62、D92—D110、D222—D230）：**

- [ ] 文字为 1—4,096 UTF-8 字节；Emoji、LF、非法控制字符和超限输入均符合合同。
- [ ] “已发送”只在 MySQL 提交后显示；5 秒无确认变失败，点击失败图标沿用原 `local_id`。
- [ ] 推送、历史和重复补收按 `message_id` 合并，delivery 缺口不能被跳过确认。
- [ ] 新消息更新预览、未读并置顶会话，但不自动切换当前聊天对象。
- [ ] 清空只推进操作账号水位，不删共享正文、不影响对方；不确定发送结果存在时禁止清空。
- [ ] 真实 MySQL 与 TLS 下完成 100 在线、每秒 50 条持续 10 分钟的规定测试，并记录硬件、成功率、重复／丢失和 P95。

**简历证据包**：消息可靠性时序、断线补收演示、故障矩阵和可复现性能报告。

### M6／W11—W13 · 加密缓存、同步状态机与正式 Qt UI

| 周次 | 顺序能力点 | 关键概念／API | 本周交付 | 状态 |
| --- | --- | --- | --- | --- |
| W11 | M6-1 profile 隔离 → M6-2 SQLite 线程 → M6-3 XChaCha20 加密 → M6-4 migration／损坏恢复 | `QSqlDatabase`、QThread、XChaCha20-Poly1305、DPAPI CurrentUser、WAL、cache quota | 每服务器／账号／profile 的加密缓存、草稿和恢复 | ❌ |
| W12 | M6-5 AppController → M6-6 SyncCoordinator → M6-7 Reconciliation／ChangeSet → M6-8 认证页面 | 单向数据流、generation、queued connection、Qt validators、异步 UI 状态 | 登录同步、崩溃收敛、登录／注册／重置完整界面 | ❌ |
| W13 | M6-9 会话／联系人 model → M6-10 消息 delegate → M6-11 托盘／多 profile → M6-12 DPI／IME／可访问性 QA | `QAbstractListModel`、delegate、QPainter、QSystemTrayIcon、单实例锁、QPointer | 正式三栏主界面、气泡、输入区、托盘和双客户端 UI | ❌ |

**阶段验收（ENV-01—ENV-06、UI-01—UI-04、D129—D174、D196、D204—D205、D213、D219—D225）：**

- [ ] 每次启动必须先在线登录；不同服务器、账号和 profile 不能读到彼此缓存或凭据。
- [ ] SQLite 只在专用 QThread 使用，UI／网络线程不直接操作数据库连接。
- [ ] 断线保留界面并显示“连接已断开”，暂停发送、自动重连，恢复后先补收再 Ready。
- [ ] Enter 发送、Shift+Enter 换行、IME 不误发；发送中／已发送／失败三态正确。
- [ ] 当前会话、后台、托盘和查看旧消息时的未读推进符合合同。
- [ ] 100%／125%／150%／200% DPI、键盘焦点、浅色主题和双 profile 人工 QA 有记录。

**简历证据包**：状态流图、缓存威胁边界、正式 UI 截图／视频、DPI 矩阵和双 profile 演示。

### M7／W14—W15 · Windows 交付、总验收与求职材料

| 周次 | 顺序能力点 | 关键概念／API | 本周交付 | 状态 |
| --- | --- | --- | --- | --- |
| W14 | M7-1 Windows 服务 → M7-2 admin／诊断 → M7-3 QtIFW 安装 → M7-4 CI／发布审查 | SCM、ACL、ProgramData、windeployqt、Qt Installer Framework、CTest labels、GitHub Actions | Server／Client 离线安装包、服务、admin CLI、脱敏诊断和 CI | ❌ |
| W15 | M7-5 42 条追踪 → M7-6 干净机 E2E → M7-7 性能／UI 总验收 → M7-8 README／演示／简历复盘 | traceability、release checklist、故障矩阵、架构图、STAR／问题复盘 | v1.2 验收包、发布说明、演示视频／脚本、简历项目材料和完整交接 | ❌ |

**阶段验收（D175—D185、D231）：**

- [ ] 干净 Windows 11 x64 在断网条件下安装 MySQL、服务端、私有 CA 和两个客户端 profile，并完成全流程。
- [ ] 单元、合同、真实 MySQL、TLS、Qt SQLite、双 profile E2E、故障、安全、性能和 UI QA 均有新鲜证据。
- [ ] 安装包不包含数据库、缓存、日志、密码、令牌、私钥、构建产物或学习笔记。
- [ ] 42 条需求和 D01—D231 均能追溯到实现与测试；未验证项不能写成完成。
- [ ] 用户能在不看稿时讲清架构、关键数据流、三个真实故障、两个取舍和未来文件扩展边界。

**简历证据包**：一键复现说明、Release Checklist、干净机录像、性能图表、架构图和只陈述已验证事实的简历条目。

### 首版后能力索引（不属于 W0—W15）

| 顺序 | 能力 | 启动门禁 | 状态 |
| --- | --- | --- | --- |
| P1 | 文件传输 | M0—M7 全部通过后，重新确认协议、断点、取消、期限、清理和 100 MiB 验收 | 🔒 |
| P2 | 语音消息 | P1 的 blob／数据连接管线稳定后，另行确认录制、编码、播放和设备权限 | 🔒 |
| P3 | 语音转文字 | P2 后单独评估离线引擎、模型大小、中文准确率和部署成本 | 🔒 |

---

## 5. 每周工作模板

### 5.1 每天 8 小时固定分配

| 时长 | 内容 | 主责 | 产出 |
| --- | --- | --- | --- |
| 1.5 h | 读取本步合同，学习新概念、API、数据流、失败和边界 | AI 教学，用户复述 | API 清单、时序／状态说明 |
| 0.5 h | 不超过 30 行的最小实验和当天文件／函数计划 | AI 给骨架，用户完成变式 | 可运行实验、当天验收点 |
| 3.5 h | 编写当前唯一能力点的生产成功路径和必要错误处理 | 用户主写，AI 实时答疑／审查 | 小步生产代码、必要编译结果 |
| 1.25 h | 在生产接口稳定后补单元、合同或集成测试 | AI 直接编写测试，用户审阅关键断言 | 能因正确原因失败／通过的测试 |
| 0.75 h | 运行测试、定位首个失败、修复或记录环境阻塞 | AI 诊断，用户处理生产逻辑 | 命令、退出码、失败数和根因 |
| 0.5 h | 范围审查、唯一学习笔记、进度和 Git 边界记录 | AI 整理，用户确认理解 | 当日记录和下一唯一待办 |

每日合计 8 小时。AI 响应等待、依赖下载或长测试不自动算有效学习时间；空档用于复盘、阅读源码或整理验证证据，不能借机开启第二个能力点。

### 5.2 每周 56 小时节奏

| 日程 | 重点 | 目标 |
| --- | --- | --- |
| 第 1—2 天 | 概念、最小实验、核心模型和成功路径 | 建立能运行的最小纵向切片 |
| 第 3—4 天 | 完成生产行为、边界和持久化／线程合同 | 功能代码达到可测试状态 |
| 第 5 天 | AI 集中补测试，用户审阅断言并修复生产问题 | 自动验证通过且测试没有镜像实现 |
| 第 6 天 | 真实依赖集成、故障注入、人工 UI／双客户端验收 | 获得运行证据和失败边界 |
| 第 7 天 | 解释检查、重构审查、文档、演示素材、缓冲返工 | 更新掌握度和简历证据包 |

### 5.3 AI 与用户职责

| 工作 | AI | 用户 |
| --- | --- | --- |
| 概念与 API | 必须先讲用途、签名、输入输出、顺序、失败和常见误用 | 用自己的话复述并完成最小变式 |
| 任务拆分 | 细化到文件、类、函数、变量、实现顺序和验收 | 确认当前只推进一个能力点 |
| 生产代码 | 默认给思路、骨架、审查和逐层提示；只有明确要求实现／修改／修复才代写 | 默认亲自实现并解释关键路径 |
| 自动化测试 | 已授权 AI 为当前能力点直接设计和编写测试、fake、夹具与测试脚本 | 审阅关键断言、隔离方式和失败原因 |
| 调试 | 先复现、缩小边界、提出可证伪假设并读取真实输出 | 提供环境事实，修改生产根因而非削弱测试 |
| 人工验收 | 给出场景、观察点和记录模板 | 亲自操作 UI／双客户端并确认体验 |
| 文档与记录 | 同步需求追踪、路线、唯一学习笔记和证据摘要 | 确认表述没有超过真实完成度 |
| Git | 只在明确授权后按文件暂存、提交或推送 | 决定提交范围与发布时间 |

AI 补测试的授权不包含擅自重写生产实现。若为了可测试性需要改变公共接口、依赖方向或生产所有权，先把所需改动讲清，由用户实现或另行明确授权。AI 编写的测试不能直接提升用户掌握等级；用户至少要能说明“这个测试保护哪个合同、怎样构造反例、为什么会失败”。

每个能力点按这个顺序推进：

```text
需求与不变量
  → 新概念与 API
  → 最小实验
  → 生产成功路径
  → 失败／边界路径
  → 回归测试与日志
  → 人工验收
  → 能改、能讲、能迁移
  → 文档和 Git 范围审查
```

教学时必须给出：问题位置、输入输出、数据结构、调用顺序、失败方式、边界、具体文件／函数／变量、实现顺序和验收条件。默认由用户写生产代码；只有用户明确要求“帮我修改／实现／修复”才代写。

---

## 6. 仓库与学习产出规范

### 目标目录

```text
D:\全栈聊天软件\
├── client-qt/             # Qt Widgets 应用与客户端 adapters
├── chat-server/           # Asio 模块化单体服务端
├── shared/                # 纯 C++20 contracts、值对象与协议描述
├── db/migrations/         # MySQL 版本迁移
├── tools/admin/           # chathub-admin.exe
├── tests/                 # 单元、合同、集成、E2E、性能
├── packaging/             # Client／Server 离线安装
├── assets/                # 默认头像、SVG 图标和 UI 资源
└── docs/                  # 产品、设计、部署、验收与项目规划
```

### 学习资料边界

- 本路线保存在 `docs/项目规划/`，但只作为本地学习事实源，不进入 Git。
- 唯一学习笔记位于 `C:\Users\Administrator\Documents\CodexLearning\全栈聊天软件\学习笔记.md`，始终在工程外。
- 项目 Git 只保存产品、协议、架构、部署、验收等交付文档，以及本次相关源码、配置和测试。
- 暂存时必须列出明确文件，不使用 `git add .`；数据库、秘密、证书私钥、日志、构建目录和本地笔记永不提交。

### 提交建议

| 前缀 | 用途 | 示例 |
| --- | --- | --- |
| `feat:` | 已确认功能 | `feat: add two-phase registration handler` |
| `fix:` | 有证据的缺陷修复 | `fix: preserve delivery gap during reconnect` |
| `test:` | 测试与验收工具 | `test: cover duplicate local message id` |
| `refactor:` | 行为不变的结构调整 | `refactor: isolate protocol dispatcher` |
| `docs:` | 项目交付文档 | `docs: record private CA deployment` |
| `chore:` | 构建与依赖 | `chore: add mingw vcpkg preset` |

每完成一个 M 阶段再决定是否建立里程碑 tag；不能只因时间到达就打完成标签。

---

## 7. 协议、安全与质量红线

### 7.1 协议红线

| 约束 | 固定合同 |
| --- | --- |
| 帧头 | magic 2B＋version 1B＋type 1B＋body length 4B，均按已确认字节序 |
| JSON 上限 | 65,536 字节，不含帧头和 TLS 开销；先校验长度再分配 |
| 文本上限 | 1—4,096 UTF-8 字节，不以 `QString::length()` 替代字节数 |
| 身份 | 64 bit ID／序号在线上使用规范十进制字符串 |
| 请求关联 | `request_id` 只关联请求响应，不能代替 `local_id` |
| 消息幂等 | 同一认证发送者＋`local_id` 唯一；冲突复用必须拒绝 |
| 写队列 | 同一连接禁止并发 async write；队列必须有帧数和字节上限 |
| 未知内容 | 安全落库、显示升级占位、推进 delivery；禁止发送、打开或执行 |

### 7.2 安全红线

- TLS 校验失败不得忽略；客户端只信任项目私有 CA，不修改 Windows 全局根证书库。
- 密码和重置码使用 Argon2id；会话 token 只保存快速摘要，二者用途不能混淆。
- 服务器受信任且能读取正文；不得把 TLS 或客户端缓存加密宣传为端到端加密。
- 日志禁止记录密码、重置码、token、密钥、消息正文和完整数据库错误秘密。
- 权限由认证上下文和数据库状态决定，不信任客户端声明的账号、好友或管理员身份。

### 7.3 完成红线

- SKIP、未运行和旧二进制结果都不算通过；构建 active IDE 目录后再信任 CTest。
- mock 只能证明端口调用，不能代替真实 MySQL、TLS、SQLite 或双客户端验收。
- 发现公共协议、持久数据或验收合同冲突时先停止实现并回到设计确认。
- 每轮结束必须报告：当前能力点、已完成项、下一待办项、范围审查、新鲜验证和掌握状态。

---

## 8. 正确使用旧 ChatHub 与外部参考

| 来源 | 可以参考 | 不能直接采用 |
| --- | --- | --- |
| `D:\CppLearn\chathub` | 已有 CMake、Qt、Asio、测试、MySQL 集成和 UI 调用链 | 旧 Node auth、旧协议、旧目录耦合、旧状态模型不能自动成为新合同 |
| llfcchat | 登录、好友、单聊和 Qt／Asio 功能地图 | 邮箱验证码、微服务、Redis 全局管理器和具体协议不符合本项目 |
| Telegram Desktop | 大型桌面客户端的组织与 Model/View 思路 | Telegram 服务端协议和规模假设不适用于局域网单体 |

参考步骤固定为：先写本项目合同和自己的方案，再定位参考代码，最后记录差异。禁止从参考仓库复制整段架构后反向修改需求。

---

## 9. 全技术栈清单

> `首版必须` 表示 M0—M7 的交付门禁；`首版后` 不阻塞 v1.2。状态只基于新项目证据。版本已由 D01—D231 固定的必须照用；测试框架等路线建议须在 M0 用 MinGW 实测后锁入 manifest，失败时记录替代方案而不是静默换栈。

### 9.1 语言、工具链与依赖管理

| 技术 | 项目用途 | 选择／版本 | 优先级 | 状态 |
| --- | --- | --- | --- | --- |
| C++20 | 客户端、服务端、shared、admin 和测试统一语言 | 禁止编译器扩展；不为 `std::expected` 升 C++23 | 首版必须 | 🟡旧项目／❌新基线 |
| Qt 6.11.1 MinGW 64-bit | Windows GUI 和 Qt 运行时 | Online Installer 对应 MinGW 套件 | 首版必须 | ❌ |
| MinGW-w64 GCC／GDB | 编译、链接和调试 | 精确版本在 M0 从 Qt kit 记录 | 首版必须 | 🟡 |
| CMake＋Ninja | target 构建和增量编译 | M0 固定最低 CMake 版本，使用 Presets | 首版必须 | 🟡 |
| manifest vcpkg | C／C++ 第三方依赖锁定 | 固定 baseline commit，triplet=`x64-mingw-dynamic` | 首版必须 | 🟡 |
| Git＋GitHub | 版本、代码审查和简历仓库 | 产品运行不依赖互联网；秘密和本地资料不上传 | 首版必须 | 🟡 |
| CLion 或 Qt Creator | 编辑、断点和调用栈 | IDE 可选，不写入构建合同 | 辅助 | 🟡 |
| clang-format／clang-tidy | 格式和高价值静态检查 | 规则在 M0 锁定，警告不等于全部自动修复 | 首版必须 | 🟡 |

### 9.2 现代 C++ 与架构模式

| 技术／模式 | 项目落点 | 掌握要求 | 优先级 | 状态 |
| --- | --- | --- | --- | --- |
| RAII、值语义、Rule of Zero/Five | MySQL、TLS、线程、句柄和临时资源 | 能解释析构、移动和异常安全 | 首版必须 | 🟡 |
| `unique_ptr/shared_ptr/weak_ptr` | 普通独占、Asio Session、弱回调 | 不用全局 shared_ptr 掩盖所有权 | 首版必须 | 🟡 |
| `variant/optional/chrono/filesystem` | payload、Outcome、时限、路径和配置 | 能写边界测试并处理空值／时钟 | 首版必须 | 🟡 |
| 强类型 ID／值对象／`enum class` | AccountId、MessageId、DeliverySeq 等 | 不同身份和序号不可隐式互转 | 首版必须 | ❌ |
| 模块化单体＋Ports/Adapters | 客户端和服务端主架构 | 依赖只向内，由 CMake 和扫描强制 | 首版必须 | ❌ |
| Composition Root＋构造注入 | bootstrap 组装 ports 和 adapters | 禁止 Service Locator 和隐藏全局状态 | 首版必须 | ❌ |
| Command Handler＋ProtocolDispatcher | 每个服务端用户操作 | Session 不含业务、handler 不接触 socket／SQL | 首版必须 | ❌ |
| Repository＋TransactionRunner | MySQL 访问和事务边界 | 提交后才返回成功和事件 | 首版必须 | ❌ |
| Presentation Model＋Model/View/Delegate | Qt 单向状态流和列表 UI | QWidget 只发 intent 和渲染快照 | 首版必须 | 🟡概念／❌新架构 |
| Strategy Registry | 消息 presenter 和未来 compose action | 静态注册，不做 DLL 插件 | 首版必须 | ❌ |
| Branch by Abstraction | 从旧工程渐进迁移 | 每次替换后旧基线仍可构建／回退 | 首版必须 | ❌ |
| C++20 coroutine | 回调模型稳定后的独立实验 | 不阻塞 v1.2 | 首版后 | ❌ |

### 9.3 Qt 6 客户端

| Qt 模块／API | 项目用途 | 关键边界 | 优先级 | 状态 |
| --- | --- | --- | --- | --- |
| Qt Core | QObject、信号槽、事件循环、QTimer、QSettings、QStandardPaths | parent ownership、线程归属、queued connection | 首版必须 | 🟡 |
| Qt Gui／Widgets／Designer | 登录注册、三栏主窗口、输入和对话框 | 正式交互与浅色设计，不复制微信资产 | 首版必须 | 🟡原型／❌正式 UI |
| Qt Network／`QSslSocket` | TLS 控制长连接、证书和重连 | 只有 `encrypted()` 后发送业务；不忽略 sslErrors | 首版必须 | ❌ |
| Qt Sql／QSQLITE | 本地缓存、迁移和事务 | 专用 QThread 独占连接 | 首版必须 | 🟡 |
| Qt Svg／资源系统 | 自有 SVG 图标和 12 个默认头像 | DPI 适配，不依赖外网资源 | 首版必须 | ❌ |
| `QAbstractListModel`／delegate／QPainter | 联系人、会话和消息气泡 | 避免每项 QWidget，使用稳定 peer／message 身份 | 首版必须 | 🟡 |
| QThread worker-object | NetworkWorker、CacheWorker 和未来后台任务 | 跨线程只传不可变 DTO／值对象 | 首版必须 | 🟡 |
| QSystemTrayIcon | 后台接收、闪烁、悬停未读和退出 | 关闭窗口不退出，托盘菜单“退出”才结束 | 首版必须 | ❌ |
| Qt Test | Qt model、controller、signal 和关键交互测试 | 不用脆弱像素截图替代状态断言 | 首版必须 | ❌ |
| IME／DPI／Accessibility API | 中文输入、100%—200% DPI、键盘与基础无障碍 | 人工 QA 与可自动状态检查分开 | 首版必须 | ❌ |
| QML／Qt Quick | 不进入当前 Widgets 路线 | 只有未来明确换 UI 技术时另立项 | 不采用 | — |

### 9.4 网络、协议与 C++ 服务端

| 技术 | 项目用途 | 关键边界 | 优先级 | 状态 |
| --- | --- | --- | --- | --- |
| TCP／IPv4 | 局域网可靠字节流和端口连接 | 首版不支持 IPv6；一次 read 不是一帧 | 首版必须 | 🟡 |
| standalone Asio | acceptor、socket、timer、executor 和异步 Session | 不使用 Boost.Asio；网络线程不阻塞 | 首版必须 | 🟡 |
| strand／`post`／`dispatch` | 串行化 Session 状态 | 明确 executor、回调生命周期和关闭顺序 | 首版必须 | 🟡 |
| OpenSSL 3.5 LTS／Asio SSL | 服务端 TLS、证书和加密传输 | 与 Qt TLS runtime 来源匹配，不混 DLL | 首版必须 | ❌ |
| 私有 CA／ECDSA P-256／SAN | 局域网服务器身份验证 | 根私钥离线；客户端仅应用内信任 CA | 首版必须 | ❌ |
| 8 字节应用帧 | magic、version、type、body length | 大端、64 KiB、先校验再分配 | 首版必须 | 🟡旧协议／❌新合同 |
| Qt JSON＋Boost.JSON | 客户端／服务端紧凑 UTF-8 JSON codec | JSON 类型不进入 domain/application | 首版必须 | 🟡 |
| UUID v4／规范十进制 ID | request／local identity 和 64 bit 线上值 | `request_id`、`local_id`、`message_id` 不混用 | 首版必须 | 🟡 |
| hello capability／连接状态机 | 协议协商、认证前后权限和未来内容兼容 | 未知 kind 不得卡住 delivery | 首版必须 | ❌ |
| 独立 TLS 数据连接＋ticket | 未来文件／语音二进制流 | 不把 100 MiB 内容放进 JSON 控制帧 | 首版后 | ❌ |

### 9.5 数据库、缓存与数据建模

| 技术 | 项目用途 | 关键边界 | 优先级 | 状态 |
| --- | --- | --- | --- | --- |
| MySQL Community 8.4.11 LTS | 账号、关系、消息、session、delivery 权威数据 | 只由 C++ 服务端访问；首版不做备份系统 | 首版必须 | 🟡旧项目／❌新 schema |
| MariaDB Connector/C／libmariadb | MinGW 下访问 MySQL | RAII、prepared statement、验证 `caching_sha2_password` | 首版必须 | 🟡 |
| SQL DDL／索引／外键／唯一约束 | 数据不变量和幂等 | `(sender, local_id)` 等约束必须由数据库保护 | 首版必须 | 🟡 |
| 事务隔离／锁顺序／deadlock | 好友、消息、delivery 和认证一致性 | `READ COMMITTED` 写事务，安全重试仅限明确回滚 | 首版必须 | 🟡 |
| 版本化 SQL migration | schema 向前演进 | 显式 `--migrate`，正常启动只校验 | 首版必须 | 🟡 |
| SQLite／QSQLITE | 客户端可重建缓存、草稿和待定消息 | 服务器是事实源，每次启动先在线认证 | 首版必须 | 🟡 |
| Wire／Domain／Record／View 模型 | 分开网络、业务、持久化和展示 | 显式 mapper，不用一个 struct 穿透所有层 | 首版必须 | ❌ |
| cursor／watermark／projection | 历史、未读、清空、列表排序和补收 | 不使用时间戳或自增 ID 代替所有序号 | 首版必须 | ❌ |
| Redis | 首版后可替换限流实验 | 首版 MySQL 持久化，不做双写 | 首版后 | 🟡旧项目／❌新项目 |

### 9.6 认证、安全与 Windows 平台

| 技术／API | 项目用途 | 关键边界 | 优先级 | 状态 |
| --- | --- | --- | --- | --- |
| libsodium Argon2id | 密码和重置码哈希 | 目标机实测成本；原值不落库不进日志 | 首版必须 | ❌ |
| libsodium CSPRNG／Base32 | token、重置码和随机 ID | 使用安全随机源，重置码排除易混字符 | 首版必须 | ❌ |
| BLAKE2b token digest | 可索引 session token 摘要 | 不能用快速摘要存密码 | 首版必须 | ❌ |
| XChaCha20-Poly1305 | SQLite 正文、事件、草稿和失败消息加密 | nonce 唯一、AEAD 校验失败不返回伪正文 | 首版必须 | ❌ |
| Windows DPAPI | 缓存密钥和服务数据库秘密保护 | CurrentUser／LocalMachine scope 按场景区分 | 首版必须 | ❌ |
| Windows Credential Manager | “记住密码” | TargetName 绑定 profile、服务器和账号 | 首版必须 | ❌ |
| Windows ACL／DACL | ProgramData 配置、秘密和 blob 权限 | 管理员／SYSTEM 不在正文保密威胁模型外 | 首版必须 | ❌ |
| SCM／Windows Service API | ChatServer 安装、启动、停止和恢复 | 启动校验全部通过后才监听 | 首版必须 | ❌ |
| Windows Firewall／专用网络 | 只开放局域网服务端端口 | MySQL 不向局域网开放 | 首版必须 | ❌ |
| 输入校验／限流／稳定错误码 | 认证、好友、消息和管理操作 | 类型、长度、编码、身份和权限都要验证 | 首版必须 | 🟡 |
| 信任局域网服务器模型 | 明确管理员可读服务端正文 | TLS 不能宣传为 E2EE | 首版必须 | ✅设计／❌部署说明 |

### 9.7 自动化测试、调试与质量工具

| 技术／实践 | 项目用途 | AI 与用户边界 | 优先级 | 状态 |
| --- | --- | --- | --- | --- |
| CTest | 统一发现、标签、运行和报告 | AI 维护测试入口，用户会读失败和 SKIP | 首版必须 | 🟡 |
| GoogleTest／GoogleMock（路线建议） | 纯 C++ domain、handler、mapper、port fake | M0 先验证 MinGW/vcpkg；通过后锁版本 | 首版必须 | ❌待 M0 固定 |
| Qt Test／QSignalSpy | Model、Controller、Qt signal 和线程边界 | AI 写测试，用户解释事件循环与断言 | 首版必须 | ❌ |
| Repository contract tests | fake、MySQL 和 SQLite 实现共享合同 | fake 通过不能替代真实实现 | 首版必须 | ❌ |
| TLS／MySQL／进程级集成测试 | 真实依赖和启动边界 | 使用临时 schema、端口和证书 | 首版必须 | 🟡旧项目／❌新项目 |
| 双 profile E2E | 同机两个客户端的真实用户流程 | AI 写驱动／探针，用户完成 UI 人工观察 | 首版必须 | ❌ |
| 故障注入 | 断线、慢客户端、MySQL 写失败、低磁盘和依赖失效 | 每个故障验证无伪成功和无关会话存活 | 首版必须 | 🟡旧项目／❌新项目 |
| 负载生成器／统计 | 100 在线、50 条／秒、10 分钟和 P95 | AI 补工具，用户解释口径和机器限制 | 首版必须 | ❌ |
| GDB／调用栈／日志关联 | 编译、崩溃、异步和线程问题 | 先首个错误和可证伪假设，不随机改代码 | 首版必须 | 🟡 |
| AddressSanitizer／UBSan | 可用工具链下检查越界和未定义行为 | M0 spike；与 Qt/MinGW 不兼容时记录限制 | 尽量完成 | ❌待验证 |
| clang-tidy／编译警告 | 生命周期、转换和常见缺陷 | 只修高价值问题，不追求零噪声 | 首版必须 | 🟡 |
| AI 测试审查清单 | 防止镜像实现、弱断言和假集成 | 用户审阅关键断言后才计入证据 | 首版必须 | ❌ |

### 9.8 可观测性、CI、安装与发布

| 技术／产物 | 项目用途 | 关键边界 | 优先级 | 状态 |
| --- | --- | --- | --- | --- |
| 结构化 JSON 滚动日志 | 客户端／服务端阶段、对象和错误关联 | 自有薄适配器；正文和秘密禁止记录 | 首版必须 | 🟡旧项目／❌新项目 |
| 脱敏诊断包 | 本地排错和简历演示 | 只含允许日志和公开环境信息，不自动上传 | 首版必须 | ❌ |
| GitHub Actions | Windows MinGW 配置、构建、unit／contract 测试 | 产品不依赖互联网；真实本地秘密测试分离 | 首版必须 | 🟡旧项目／❌新项目 |
| CMake install／windeployqt | 收集 Qt 和 C／C++ 运行库 | 验证 DLL 来源与 OpenSSL backend | 首版必须 | ❌ |
| Qt Installer Framework | Client／Server 离线安装包 | 分开打包，首版不自动更新 | 首版必须 | ❌ |
| 第三方许可证清单 | 依赖合规和发布说明 | 版本、许可证和来源可追踪 | 首版必须 | ❌ |
| Release Checklist | 42 条需求、D01—D231、测试和发布范围 | SKIP／未运行不能写为通过 | 首版必须 | ❌ |
| Markdown＋Mermaid | README、架构、协议、部署和时序图 | 文档只记录已确认合同和新鲜证据 | 首版必须 | 🟡 |

### 9.9 首版不采用的技术

| 技术 | 当前决定 |
| --- | --- |
| Node.js／Express／JWT Auth Service | 旧实现只作迁移参考，最终退出新运行链路 |
| Redis | 首版不部署；仅作后续限流替换学习实验 |
| Protobuf／gRPC／WebSocket | 单体局域网控制连接没有当前必要性 |
| 微服务／消息队列／Service Mesh | 没有多实例和异步削峰需求，不为简历堆栈 |
| QML／Qt Quick | 首版固定 Qt Widgets |
| DLL／脚本插件 | 首版使用静态注册和静态库 |
| E2EE | 产品明确采用信任局域网服务器模型 |
| Docker 生产部署 | 首版服务器原生运行在 Windows；容器不作为交付前提 |

---

## 10. 简历证据主线

技术名称只有与可复现证据绑定后才进入简历。每完成一个阶段，保留以下材料：

| 阶段 | 可展示成果 | 面试可讲主题 | 禁止提前写入的表述 |
| --- | --- | --- | --- |
| M0 | 一键构建、依赖锁和迁移边界 | MinGW／CMake／vcpkg 可复现性 | “完成企业级工程化” |
| M1 | TLS 握手、分帧测试、协议图 | 半包粘包、strand、证书身份、协议状态机 | “高并发安全通信”但没有负载证据 |
| M2 | schema、事务测试、故障隔离 | RAII、连接所有权、事务提交未知、post-commit | “零数据丢失”但未完成 E2E |
| M3 | 注册登录重置和安全矩阵 | Argon2、session token、DPAPI、限流、单点登录 | “端到端加密” |
| M4 | 好友状态机和双客户端演示 | 幂等、版本冲突、双向关系和投影 | “社交系统”但只有按钮 |
| M5 | 断线补收、历史、性能报告 | 四类 ID／序号、delivery、背压、P95 | “支持 50 条／秒”但测试未通过 |
| M6 | 正式 UI 视频、缓存恢复和 DPI QA | Qt Model/View、线程归属、单向状态流、加密缓存 | “高度还原微信”或复制商标资产 |
| M7 | 安装包、干净机录像和 Release Checklist | Windows 服务、离线部署、故障矩阵和架构取舍 | “项目完成”但有未验收条目 |

最终简历条目至少包含：项目目标、个人职责、三项有量化证据的技术难点、一个架构取舍、一个故障案例和可访问的代码／演示链接。投递版本只使用已经通过验收的阶段成果，未完成内容标为计划或不写。

---

## 11. 掌握度与验证实验

| 等级 | 可观察证据 | 路线判定 |
| --- | --- | --- |
| L1 知道 | 能说出概念解决什么问题 | 未掌握 |
| L2 会用 | 能跟随最小示例完成基本调用 | 未掌握 |
| L3 会改 | 能独立修改、调试并处理边界 | 学习中的分水岭 |
| L4 会设计 | 能从需求反推方案并解释取舍 | 接近或达到掌握 |
| L5 会迁移 | 能用于相邻问题并讲给他人 | 稳定掌握 |

每个 M 阶段至少完成一次解释和迁移实验：

1. **M0**：换一台或清空构建目录后，仅依赖文档重建项目。
2. **M1**：修改一种合法帧字段或错误边界，并同步双端 codec 与测试。
3. **M2**：为一个新实体增加最小仓储和迁移，同时保持业务层无 SQL。
4. **M3**：解释一次登录成功、密码错误、旧登录被挤和账号停用的数据流。
5. **M4**：给出交叉申请与删除／重加并发场景，判断最终关系和版本。
6. **M5**：从 ACK 丢失和重连重复推送推导 `local_id`、`message_id`、delivery 水位的行为。
7. **M6**：在不改 QWidget 的情况下替换一个 fake network port，并解释 ChangeSet 如何更新 Model。
8. **M7**：在干净电脑演示后，独立讲清架构、故障、性能结果和未实现边界。

---

## 12. 如果卡住了

按顺序判断卡点，不直接索要整段答案：

1. **概念不清**：回到“它解决什么、输入输出、状态与失败”。
2. **API 不会**：提供签名、参数、返回／错误、调用前提和不超过 30 行示例。
3. **调用顺序不清**：画同步／异步时序和线程归属。
4. **编译失败**：保留完整首个错误、构建目录和实际命令，先排工具链与链接边界。
5. **测试失败**：区分生产错误、测试预期过时和环境问题，不削弱断言求绿。
6. **连续三次修复无效**：停止打补丁，重新审查前提、状态所有权和复现方法。

用户卡住时，教练先补缺失的一层；除非用户明确要求代写，不直接覆盖生产实现。

---

## 13. 学习记录模板

每完成一个能力点，在本路线追加一条简短记录，并在工程外唯一学习笔记保存详细理解：

```text
日期：
能力点：M?-?
产品／设计追踪：REQ-??；D??—D??
代码状态：未实现／已实现未验证／自动验证通过／人工验收通过
新鲜验证：命令、退出码、通过／失败数、人工观察
我能解释：
我仍不懂：
掌握等级：L1—L5
范围审查：本次文件；保留的无关改动；未进入 Git 的资料
下一待办：等待“下一步”后推进的唯一能力点
```

### 当前记录

- **2026-09-01**：v1.2 的 42 条需求和 D01—D231 已收敛；按每天 8 小时、每周 56 小时重排为 W0—W15 全职路线，补齐技术栈、AI 协作和简历证据线。当前只有规划证据，M0—M7 均未开始。下一能力点为 M0-1，等待用户说“下一步”。
- **2026-09-01 上下文工程**：已建立中文 `AGENTS.md`／`CLAUDE.md`、`INITIAL.md`、`TASK.md`、能力点 PRP 模板、M0-1 示例、生成／执行／验证流程和分层验证矩阵。它只改进 AI 上下文与工作流，不改变产品合同或启动 M0-1。

---

## 14. 成功标准

首版只有同时满足以下条件才算完成：

- [ ] 42 条产品需求和 D01—D231 均有实现、自动测试或人工验收追踪。
- [ ] Windows 局域网断网环境完成注册、登录、重置、好友和文字单聊全流程。
- [ ] 真实 MySQL／TLS／SQLite、双 profile、故障、安全、性能、UI 和干净安装证据齐全。
- [ ] 代码遵守确认的模块依赖，新增内容类型的架构测试不迫使 Session、FrameDecoder 或主窗口重写。
- [ ] 文件、语音、群聊、E2EE、Redis 等首版外能力没有被包装成已实现。
- [ ] 用户至少对各阶段关键能力达到 L4，并能对核心网络、事务、同步、线程和安全边界完成一次迁移解释。
- [ ] README、部署、测试、演示、交接和简历描述只陈述有新鲜证据的事实。

达到这些条件后，项目状态才可从“学习中的功能实现”提升为“可演示、可解释、可复现的首版作品”。
