# W8-2 需求文档：QQ 风格 Qt 聊天窗口

> 日期：2026-08-05  
> 项目：ChatHub V1  
> 阶段：W8 切片 2 - 私聊窗口  
> 状态：设计已确认，待按模块实现与验收

---

## 1. 目标与范围

程序先以模态 `LoginDialog` 完成注册、HTTP 登录和 TCP/JWT 认证；认证成功后关闭登录对话框，再创建类似 QQ 桌面端的
`MainWindow` 聊天窗口。两个已认证用户可以一对一发送文本消息；消息采用左右气泡显示，并在网络或输入异常时给出明确提示。

本切片追求的是 **QQ 的信息层级与交互感**，不是复刻 QQ 的品牌、图标或全部功能。

### 本切片必须完成

- `LoginDialog` 作为应用入口；只有认证成功才创建并显示 `MainWindow`。
- `ChatClient` 的生命周期覆盖登录对话框和聊天主窗口，登录框关闭后 TCP 连接保持可用。
- 左侧导航、会话列表、中间聊天区的三栏布局。
- 选择/输入接收者后发送一对一文本消息。
- 自己发送的消息右对齐、对方消息左对齐，并显示时间。
- 服务端依据认证身份补齐 `from`，客户端不得自行声明发送者身份。
- 处理空输入、接收者不存在或离线、TCP 断线、认证失效、协议/JSON 错误。

### 本切片明确不做

- 好友关系、服务端在线列表、群聊、图片/文件、语音、历史消息持久化、撤回、已读回执。
- 真实 QQ Logo、头像下载、皮肤系统。

> 当前服务端没有“联系人/在线列表”协议，UI 不能伪造在线状态。W8 中的会话列表是**本次运行期本地会话列表**：发送或收到某用户消息后才出现。

---

## 2. 当前代码基线与必须先对齐的契约

| 位置                   | 当前状态                    | 本切片要求                                                       |
|----------------------|-------------------------|-------------------------------------------------------------|
| `MainWindow`         | 当前承载登录控件                | 改为只承载聊天界面；窗口允许调整大小                                          |
| `LoginDialog`        | 尚未独立存在                  | 承载用户名、密码、登录、注册、HTTP 提示与认证结果                                 |
| `main.cpp`           | 直接创建并显示 `MainWindow`    | 创建应用级 `ChatClient`，先 `exec()` 登录框，成功后再创建聊天窗口                |
| `ChatClient`         | 能认证、能处理 auth/error      | 新增发送 chat、解析 chat、发送聊天信号                                    |
| `chat_payload`       | 只校验 `content`           | 同时校验 `to`、`content`，并拒绝 `from`/`sender_id`                  |
| `Server::sendToUser` | 直接取 `to`，并只转发 content   | 必须使用校验结果；由认证映射补齐 `from`，再转发完整 JSON                          |
| `LoginDialog`        | 槽函数命名符合自动连接时不得再手动重复连接按钮 | 二选一：保留自动连接时删除手动 `connect(loginBtn/registerBtn)`，避免一次点击发两次请求 |

### 消息协议：唯一真相

客户端发送的 chat 帧：

```json
{
  "to": "bob",
  "content": "你好"
}
```

服务端转发给接收者的 chat 帧：

```json
{
  "from": "alice",
  "to": "bob",
  "content": "你好",
  "sentAt": "2026-08-05T10:30:25Z"
}
```

协议头保持既有格式：`[magic:2][version:1][type:1][length:4 big-endian][body]`，其中 chat 的 `type=1`。

### 输入与长度约束

| 字段                   | 客户端预校验                                   | 服务端最终校验                               |
|----------------------|------------------------------------------|---------------------------------------|
| `to`                 | 去除首尾空格；3-20 个 ASCII 字母、数字或下划线            | 同样校验；确认该用户在 `m_username_to_session` 中 |
| `content`            | 去除首尾空白后不能为空；最多 300 个字符且 UTF-8 不超过 768 字节 | UTF-8 不超过 1024 字节；非空；必须是字符串           |
| `from` / `sender_id` | 客户端绝不发送                                  | 若请求中出现，返回业务错误，不转发                     |

`768` 是客户端的体验上限，为 JSON 字段与协议体预留空间；服务端仍以协议上限作为最终安全边界。

---

## 3. QQ 风格界面方案

### 3.1 总体结构

聊天页使用 `QSplitter`，不使用固定宽高。窗口建议最小尺寸为 `960 x 640`，默认尺寸为 `1120 x 720`。

```text
┌───────┬──────────────────┬──────────────────────────────────────────┐
│ 导航栏 │ 会话栏           │ 聊天区                                   │
│ 56 px │ 220 px           │ 自适应                                   │
│       │                  │  对话标题：头像 名称 在线/离线标记       │
│ 头像  │ 搜索（本轮禁用） │ ─────────────────────────────────────── │
│ 会话  │ 会话列表         │  QScrollArea + 消息气泡容器              │
│ 设置  │                  │                                          │
│       │                  │ ─────────────────────────────────────── │
│       │                  │  接收者输入 | 多行消息输入 | 发送按钮    │
└───────┴──────────────────┴──────────────────────────────────────────┘
```

### 3.2 控件树与对象名

```text
main.cpp
├─ ChatClient chatClient                # 应用级对象；不属于任一窗口
├─ LoginDialog loginDialog(&chatClient)
│  ├─ QLineEdit userNameEdit
│  ├─ QLineEdit pwdEdit
│  ├─ QPushButton loginBtn
│  ├─ QPushButton registerBtn
│  └─ QLabel messageLabel
└─ MainWindow chatWindow(&chatClient, username)  # 仅在登录框 Accepted 后创建
   └─ QHBoxLayout chatRootLayout
      ├─ QWidget navPanel                # 固定 56 px
      │  ├─ QToolButton profileBtn
      │  ├─ QToolButton conversationsBtn
      │  └─ QToolButton settingsBtn       # 本轮只显示，不实现设置页
      └─ QSplitter chatSplitter
         ├─ QWidget conversationPanel     # 建议初始 220 px
         │  ├─ QLabel currentUserLabel
         │  └─ QListWidget conversationList
         └─ QWidget conversationPage
            ├─ QWidget chatHeader
            │  ├─ QLabel peerAvatarLabel
            │  ├─ QLabel peerNameLabel
            │  └─ QLabel connectionStateLabel
            ├─ QScrollArea messageScrollArea
            │  └─ QWidget messageContainer
            │     └─ QVBoxLayout messageLayout
            └─ QWidget composerPanel
               ├─ QLineEdit recipientEdit
               ├─ QTextEdit messageEdit
               └─ QPushButton sendBtn
```

### 3.3 设计取舍

- 消息区选择 `QScrollArea + QVBoxLayout`，而不是仅用 `QListWidget::addItem(text)`：气泡可自适应宽度、可显示时间、可放发送状态，也更接近
  QQ。
- 会话列表可继续使用 `QListWidget`：本切片只需显示会话名称与最后一条消息预览，`QListWidget` 足够简单。
- `recipientEdit` 在 W8 暂时保留。点击会话列表后自动填入接收者；在没有联系人服务前，它是创建第一段私聊所必需的入口。
- 不把接收者昵称写死为“在线”。W8 只有“连接正常/服务端告知用户离线/当前未知”三种状态。
- `LoginDialog` 不拥有 `ChatClient`。若让它成为登录框子对象，对话框在作用域结束、析构或 `deleteLater()` 时会错误析构仍需存活的
  TCP 客户端；若 `ChatClient` 本身是栈对象，还可能造成 QObject 父子析构冲突。

### 3.4 启动与对象生命周期

登录窗口不是 `MainWindow` 中的一页。程序启动时只创建并显示 `LoginDialog`；`MainWindow` 必须在对话框返回 `Accepted` 后才构造。

```text
QApplication 创建
  -> main.cpp 创建 ChatClient chat_client              # 生命周期覆盖整个会话
  -> 创建 LoginDialog login_dialog(&chat_client)
  -> login_dialog.exec()                               # 模态局部事件循环，网络信号仍可正常投递
     ├─ HTTP 登录失败 / TCP 连接失败 / 认证失败
     │  -> LoginDialog 显示错误并保持打开，不创建 MainWindow
     ├─ 用户取消或关闭
     │  -> exec() 返回 Rejected，程序正常退出
     └─ HTTP 登录 + TCP 认证均成功
        -> LoginDialog::accept()
  -> 保存 login_dialog.username()
  -> 销毁 LoginDialog（其连接到自身槽的信号自动断开）
  -> 创建 MainWindow(&chat_client, username)
  -> show() + QApplication::exec()
```

- `ChatClient` 由 `main.cpp` 以应用级对象持有，不设置 `LoginDialog` 或 `MainWindow` 为父对象；它内部的 `QTcpSocket` 仍可由
  `ChatClient` 管理。
- `LoginDialog` 可以持有只服务于登录阶段的 `HttpClient`。HTTP 成功后，将规范化后的用户名保存至 `m_username`，但**不能**立刻
  `accept()`；必须等待 TCP 的 `authSucceeded()`。
- `LoginDialog::username()` 用于在对话框结束后把身份传给 `MainWindow`。认证成功信号只表示 token 已被 chat-server
  接受，不再携带或从已销毁的输入框读取用户名。
- 登录阶段由 `LoginDialog` 处理 `authFrameSent/authSucceeded/authFailed/connectionFailed`；聊天阶段由 `MainWindow`
  处理断线、聊天消息和发送状态。Qt 在接收者对象析构时会自动断开相关连接，因此不会回调到已销毁的登录框。

---

## 4. 前端视觉规范与 QSS

### 4.1 风格基调

| 元素       | 规范                                       |
|----------|------------------------------------------|
| 主背景      | `#F5F6F8`，降低纯白大面积视觉疲劳                    |
| 导航栏      | `#1F2937` 深色，突出 QQ 式窄侧栏层级                |
| 会话栏      | 白色，选中项 `#E8F1FF`                         |
| 主按钮/自己气泡 | `#1677FF`，白色文字                           |
| 对方气泡     | 白色，边框 `#E5E7EB`                          |
| 错误提示     | `#D92D20`，不得只依赖颜色表达错误                    |
| 字体       | Windows 优先 `Microsoft YaHei`，正文 10-11 pt |
| 圆角       | 面板 8 px；气泡 12 px；不要使用过度阴影                |

### 4.2 建议 QSS（保存为 `client-qt/chat_window.qss`）

```css
QMainWindow {
    background: #F5F6F8;
    font-family: "Microsoft YaHei";
    color: #1F2937;
}

QWidget#navPanel {
    background: #1F2937;
}

QWidget#conversationPanel,
QWidget#chatHeader,
QWidget#composerPanel {
    background: #FFFFFF;
}

QListWidget#conversationList {
    border: none;
    background: #FFFFFF;
    outline: none;
    padding: 6px;
}

QListWidget#conversationList::item {
    min-height: 54px;
    border-radius: 8px;
    padding: 6px 10px;
}

QListWidget#conversationList::item:hover {
    background: #F3F7FF;
}

QListWidget#conversationList::item:selected {
    background: #E8F1FF;
    color: #1677FF;
}

QTextEdit#messageEdit,
QLineEdit#recipientEdit {
    background: #FFFFFF;
    border: 1px solid #D9DEE7;
    border-radius: 8px;
    padding: 8px;
    selection-background-color: #B9D6FF;
}

QTextEdit#messageEdit:focus,
QLineEdit#recipientEdit:focus {
    border: 1px solid #1677FF;
}

QPushButton#sendBtn {
    min-width: 78px;
    min-height: 34px;
    background: #1677FF;
    color: #FFFFFF;
    border: none;
    border-radius: 8px;
    padding: 0 16px;
}

QPushButton#sendBtn:hover { background: #4096FF; }
QPushButton#sendBtn:pressed { background: #0958D9; }
QPushButton#sendBtn:disabled { background: #BFC7D5; color: #FFFFFF; }

QLabel#connectionStateLabel[status="ok"] { color: #16A34A; }
QLabel#connectionStateLabel[status="warning"] { color: #D97706; }
QLabel#connectionStateLabel[status="error"] { color: #D92D20; }
```

气泡不建议只靠 QSS 区分。每条消息使用 `ChatBubbleWidget`，由 `MessageDirection` 决定布局方向与 `objectName`：

| 方向   | 布局  | 气泡 objectName    | 视觉                     |
|------|-----|------------------|------------------------|
| 自己发送 | 右对齐 | `outgoingBubble` | 蓝底、白字、右下显示“发送中/已发送/失败” |
| 对方发送 | 左对齐 | `incomingBubble` | 白底、浅灰边框、显示昵称与时间        |
| 系统消息 | 居中  | `systemMessage`  | 灰色小字，例如“对方当前不在线”       |

### 4.3 消息气泡最小 API

```cpp
enum class MessageDirection { incoming, outgoing, system };

class ChatBubbleWidget : public QWidget
{
    Q_OBJECT
public:
    ChatBubbleWidget(MessageDirection direction,
                     const QString &sender,
                     const QString &content,
                     const QDateTime &sent_at,
                     QWidget *parent = nullptr);

    void setSendState(SendState state); // pending / sent / failed
};
```

构造函数只调度 `setupUi()`、`setupLayout()`、`applyDirectionStyle()`，不要把布局、QSS 与业务逻辑堆在一起。

---

## 5. 功能流程与信号/槽契约

### 5.1 登录、认证并创建聊天窗口

```text
LoginDialog::onLoginClicked()
  -> HttpClient 完成 HTTP 登录
  -> 校验响应中的 token，并保存 m_username
  -> ChatClient::connectWithToken(token)
  -> QTcpSocket::connected
  -> ChatClient 发送 auth(type=5) 帧
  -> 收到 auth 成功帧
  -> emit authSucceeded()
  -> LoginDialog::onAuthSucceeded()
  -> LoginDialog::accept()
  -> LoginDialog::exec() 返回 QDialog::Accepted
  -> main.cpp 创建 MainWindow(&chat_client, login_dialog.username())
  -> MainWindow::show()
```

用户名属于 HTTP 登录结果和 `LoginDialog` 的状态，认证信号只负责确认 TCP/JWT 握手成功；这与当前 `ChatClient` 的无参认证成功信号保持一致：

```cpp
signals:
    void authSucceeded();
    void authFailed(const QString &reason);
    void connectionFailed(const QString &reason);
```

若 HTTP 响应无 token、token 为空、TCP 无法连接或收到认证拒绝，`LoginDialog` 只恢复按钮状态并显示原因，**不得**调用
`accept()`，也不得创建聊天窗口。

### 5.2 发送消息

```text
sendBtn clicked 或 messageEdit 的 Ctrl+Enter
  -> MainWindow::onSendClicked()
  -> 校验 recipientEdit 与 messageEdit
  -> ChatClient::sendChatMessage(to, content)
  -> 成功进入 QTcpSocket 写缓冲
  -> emit chatMessageQueued(to, content, localId)
  -> MainWindow 添加右侧 pending 气泡、清空输入框、更新会话预览
```

`Enter` 仅换行，`Ctrl+Enter` 发送；这样多行文本不会因误按 Enter 直接发出。

### 5.3 接收消息

```text
QTcpSocket::readyRead
  -> ChatClient::processReceivedFrames()
  -> 完整 type=chat(1) 帧
  -> 校验并解析 {from, to, content, sentAt}
  -> emit chatMessageReceived(from, to, content, sentAt)
  -> MainWindow 添加左侧气泡
  -> 会话置顶并更新预览
  -> 若当前未选中该会话，显示未读圆点
```

### 5.4 建议 ChatClient 接口

```cpp
void ChatClient::sendChatMessage(const QString &to, const QString &content);
bool ChatClient::isAuthenticated() const;

signals:
    // 仅 LoginDialog 在认证阶段接收
    void authFrameSent();
    void authSucceeded();
    void authFailed(const QString &reason);
    void connectionFailed(const QString &reason);

    // 仅 MainWindow 在认证成功后接收
    void disconnected();
    void chatMessageQueued(const QString &to,
                           const QString &content,
                           const QString &local_id);
    void chatMessageReceived(const QString &from,
                             const QString &to,
                             const QString &content,
                             const QDateTime &sent_at);
    void chatSendFailed(const QString &local_id, const QString &reason);
    void protocolError(const QString &reason);
```

`chatMessageQueued` 仅表示 Qt 已接受写入缓冲，并不等于接收者已收到。V1 没有送达回执，因此气泡最终状态为“已发送到服务器”；不要显示“对方已读”。

`MainWindow` 构造时先连接聊天阶段的信号与槽，再用 `isAuthenticated()` 同步初始化 `connectionStateLabel` 和发送按钮；不要只依赖已经发生过的
`authSucceeded()` 信号来判断初始状态。

---

## 6. 错误处理与 UI 表现

| 场景                       | 处理者               | UI 表现                        | 是否保留输入   |
|--------------------------|-------------------|------------------------------|----------|
| HTTP 登录失败、响应缺少 token 或超时 | LoginDialog       | 登录框内显示原因、恢复登录/注册按钮           | 保留用户名和密码 |
| 认证阶段 TCP 连接失败或认证被拒绝      | LoginDialog       | 登录框内显示原因，不关闭对话框              | 保留用户名和密码 |
| 用户关闭或取消登录框               | main.cpp          | 不创建 MainWindow，程序正常退出        | 不适用      |
| 接收者为空/格式非法               | MainWindow        | 输入框下方红字“请输入有效用户名”            | 保留       |
| 内容为空或全空白                 | MainWindow        | 不创建气泡，提示“消息不能为空”             | 保留       |
| 内容超过限制                   | MainWindow        | 显示“消息过长”，不调用网络层              | 保留       |
| 未认证或已断线时发送               | ChatClient        | `chatSendFailed`；发送按钮禁用      | 保留       |
| 服务端返回 error 帧            | ChatClient        | 对应 pending 气泡标记“发送失败”，可复制重发  | 保留       |
| 接收者不在线                   | Server -> error 帧 | 系统消息“对方当前不在线”                | 保留       |
| JSON/协议头非法               | ChatClient        | `protocolError`，显示连接异常并断开    | 保留       |
| 认证后的连接断开                 | MainWindow        | 显示“连接已断开，请重新登录”；保留本地会话与未发送文本 | 保留       |

W8 不实现静默自动重连，避免在用户无感知时使用过期 token 建立新会话；重新登录的入口留待后续切片。所有网络回调都在 `QObject`
所在线程执行；本切片不创建额外 UI 线程。不得在 socket 回调中直接做阻塞 I/O 或长时间 JSON/文件操作。

---

## 7. 服务端路由要求

`Server::sendToUser()` 必须按以下顺序处理，不能直接 `jv.at("to")`：

1. 调用 `parseChatPayload()`，失败则仅向发送者回 `type=error`。
2. 从认证映射 `m_session_to_username[sender_id]` 取得真实发送者。
3. 从校验后的 `to` 查 `m_username_to_session`。
4. 不在线则仅向发送者回 `type=error`，原因为“接收者不在线”。
5. 生成新的转发 JSON：`from` 由服务端写入，`to/content/sentAt` 一并写入。
6. 仅向接收者对应 Session 发送 `type=chat`。

这条规则保证客户端无法伪造 `from`，也避免缺少 `to` 时抛出异常导致进程不稳定。

---

## 8. 实施顺序

一次只推进一个模块，完成当前验收后再进入下一项。

1. **窗口职责与生命周期**：新建 `LoginDialog`，把登录/注册控件和 HTTP 提示迁入；`main.cpp` 创建应用级 `ChatClient`，用
   `exec()/Accepted` 决定是否创建 `MainWindow`。
2. **协议先行**：补全 `ChatPayloadResult` 与服务端错误响应；为合法/非法 JSON 添加测试。
3. **ChatClient**：实现 `sendChatMessage()`、type=chat 解析、聊天信号，以及登录阶段与聊天阶段的状态边界。
4. **聊天窗口骨架**：`MainWindow` 只保留三栏聊天控件树；先验证销毁 `LoginDialog` 后连接仍保持可用。
5. **气泡组件**：先实现左右方向、时间与滚动到底部，再添加 pending/failed 状态。
6. **会话列表**：实现本地会话置顶、预览和未读点；不实现在线联系人服务。
7. **联调与异常**：双客户端、取消登录、认证失败、离线接收者、断线、超长文本、半包/粘包回归测试。

---

## 9. 验收标准

- [ ] 每个客户端启动时先显示模态 `LoginDialog`；HTTP 登录和 TCP 认证均成功后，登录框关闭，再创建并显示 `MainWindow`。
- [ ] 关闭或取消 `LoginDialog` 时，不创建 `MainWindow`，程序可正常退出。
- [ ] 登录框已销毁后，`ChatClient` 仍处于已认证状态；聊天窗口可显示“连接正常”并继续使用该连接。
- [ ] 聊天页为可调整的三栏布局；窗口缩放不遮挡输入区或发送按钮。
- [ ] A 给 B 发送文本，B 收到左侧气泡，显示 A 的服务端补齐身份与时间。
- [ ] A 本地立即显示右侧 pending 气泡；网络层确认写入后显示“已发送到服务器”。
- [ ] 点击会话项会切换当前接收者；新收到消息的会话置顶。
- [ ] 空白内容、非法接收者、超长内容不会发包且不会崩溃。
- [ ] B 不在线时，A 收到明确错误提示，不显示“发送成功”。
- [ ] 拔掉/关闭 chat-server 后，发送按钮禁用、已有消息不丢失、UI 不冻结。
- [ ] 客户端发送 `{ "from": "admin" }` 或 `{ "sender_id": 1 }` 时，服务端拒绝并且不转发。
- [ ] 不存在重复按钮连接：一次点击登录只产生一次 HTTP 请求。

---

## 10. 本切片的学习重点

- `QDialog::exec()/accept()/reject()`：用模态登录窗口作为应用启动门禁，同时理解局部事件循环仍会投递网络信号。
- `QObject` 生命周期：应用级 `ChatClient` 与窗口级 `LoginDialog/MainWindow` 的所有权边界，以及接收者析构后的自动断开。
- `QSplitter`：可伸缩三栏布局与最小宽度约束。
- 自定义 `QWidget` 气泡：布局方向、`objectName`、QSS 与状态更新。
- TCP 字节流：`readyRead` 不等于一条消息，必须继续复用已有分帧缓存。
- 身份边界：客户端只提交意图（`to/content`），服务端基于认证结果定义身份（`from`）。

## 常见陷阱 / 面试题

1. 为什么 `QTcpSocket::write()` 成功不能说明对方已经收到消息？
2. 为什么不能让客户端在 JSON 中自行填写 `from`？
3. 为什么会话列表不能在没有服务端在线列表协议时显示“在线”？
4. 为什么 `ChatClient` 不能作为 `LoginDialog` 的子对象？
5. 为什么 `QDialog::exec()` 不会阻塞 Qt 的网络信号与槽？
6. 为什么 `on_loginBtn_clicked()` 与手动 `connect(loginBtn, clicked, ...)` 可能造成重复请求？
