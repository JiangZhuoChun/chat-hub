#include "logindialog.h"

#include "HttpClient.h"
#include "chatclient.h"
#include "ui_logindialog.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLineEdit>
#include <QUrl>

// ==================== 模块：生命周期与登录结果 ====================
// 功能：创建界面和 HTTP 客户端，完成初始界面设置与外部信号槽连接。
LoginDialog::LoginDialog(ChatClient *chat_client, QWidget *parent)
    : QDialog(parent), ui(new Ui::LoginDialog), m_http(new HttpClient(this)), m_chat(chat_client)
{
    Q_ASSERT(m_chat != nullptr);
    ui->setupUi(this);
    setupUiState();
    connectSlots();
}

// 功能：释放 Qt 设计器创建的界面对象。
LoginDialog::~LoginDialog()
{
    delete ui;
}

// 功能：返回最近一次成功登录后保存的用户名。
QString LoginDialog::username() const
{
    return m_username;
}

// ==================== 模块：对话框关闭处理 ====================
// 功能：取消登录时断开聊天客户端，再交由父类关闭对话框。
void LoginDialog::reject()
{
    if (m_chat != nullptr)
    {
        m_chat->disconnectFromServer();
    }
    QDialog::reject();
}

// ==================== 模块：用户界面操作 ====================
// 功能：校验登录输入，发送登录 HTTP 请求并锁定重复操作按钮。
void LoginDialog::on_loginBtn_clicked()
{
    const QString username = ui->userNameEdit->text().trimmed();
    const QString password = ui->pwdEdit->text();
    if (username.isEmpty() || password.isEmpty())
    {
        showMessage(QStringLiteral("用户名和密码不能为空"));
        return;
    }

    QJsonObject body;
    body[QStringLiteral("username")] = username;
    body[QStringLiteral("password")] = password;

    m_pending_request = RequestType::login;
    setRequestButtonsEnabled(false);
    showMessage(QStringLiteral("正在登录..."));
    m_http->post(QUrl(QStringLiteral("http://localhost:3000/login")),
                 QJsonDocument(body).toJson(QJsonDocument::Compact));
}

// 功能：校验注册输入，发送注册 HTTP 请求并锁定重复操作按钮。
void LoginDialog::on_registerBtn_clicked()
{
    const QString username = ui->userNameEdit->text().trimmed();
    const QString password = ui->pwdEdit->text();
    if (username.isEmpty() || password.isEmpty())
    {
        showMessage(QStringLiteral("用户名和密码不能为空"));
        return;
    }

    QJsonObject body;
    body[QStringLiteral("username")] = username;
    body[QStringLiteral("password")] = password;

    m_pending_request = RequestType::registerUser;
    setRequestButtonsEnabled(false);
    showMessage(QStringLiteral("正在注册..."));
    m_http->post(QUrl(QStringLiteral("http://localhost:3000/register")),
                 QJsonDocument(body).toJson(QJsonDocument::Compact));
}

// 功能：响应取消按钮，执行带断开处理的 reject()。
void LoginDialog::on_cancelBtn_clicked()
{
    reject();
}

// ==================== 模块：HTTP 请求结果处理 ====================
// 功能：解析登录或注册响应；登录成功时保存用户名并发起聊天 TCP 认证。
// 失败：响应不是 JSON 对象或未返回令牌时恢复按钮并向用户说明原因。
void LoginDialog::onRequestFinish(const int status_code, const QByteArray &body)
{
    const RequestType request_type = m_pending_request;
    m_pending_request = RequestType::none;

    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    {
        setRequestButtonsEnabled(true);
        showMessage(QStringLiteral("HTTP 响应格式错误"));
        return;
    }

    const QJsonObject object = document.object();
    const QString message = object.value(QStringLiteral("message")).toString();
    const QString error = object.value(QStringLiteral("error")).toString();

    if (request_type == RequestType::login && status_code == 200)
    {
        const QString token = object.value(QStringLiteral("token")).toString();
        if (token.isEmpty())
        {
            setRequestButtonsEnabled(true);
            showMessage(QStringLiteral("登录失败：服务器没有返回 token"));
            return;
        }

        m_username = ui->userNameEdit->text().trimmed();
        showMessage(QStringLiteral("HTTP 登录成功，正在连接 chat-server..."));
        m_chat->connectWithToken(token);
        return;
    }

    setRequestButtonsEnabled(true);
    if (request_type == RequestType::registerUser && status_code == 201)
    {
        showMessage(message.isEmpty() ? QStringLiteral("注册成功") : message);
        return;
    }

    showMessage(error.isEmpty() ? QStringLiteral("请求失败") : error);
}

// 功能：清除待处理请求状态，恢复操作按钮并显示 HTTP 网络错误。
void LoginDialog::onRequestError(const QString &error)
{
    m_pending_request = RequestType::none;
    setRequestButtonsEnabled(true);
    showMessage(QStringLiteral("HTTP 请求失败：") + error);
}

// 功能：清除待处理请求状态，恢复操作按钮并显示 HTTP 超时。
void LoginDialog::onRequestTimeOut()
{
    m_pending_request = RequestType::none;
    setRequestButtonsEnabled(true);
    showMessage(QStringLiteral("HTTP 请求超时"));
}

// ==================== 模块：TCP 认证结果处理 ====================
// 功能：提示认证帧已写入客户端发送缓冲区，等待服务端认证结果。
void LoginDialog::onAuthFrameSent()
{
    showMessage(QStringLiteral("TCP 已连接，认证帧已发送，等待服务端确认..."));
}

// 功能：认证成功后恢复按钮、显示提示并以 Accepted 结果关闭对话框。
void LoginDialog::onAuthSucceeded()
{
    m_pending_request = RequestType::none;
    setRequestButtonsEnabled(true);
    showMessage(QStringLiteral("聊天服务器认证成功"));
    accept();
}

// 功能：认证失败后清除请求状态、恢复按钮并显示服务端原因。
void LoginDialog::onAuthFailed(const QString &reason)
{
    m_pending_request = RequestType::none;
    setRequestButtonsEnabled(true);
    showMessage(QStringLiteral("聊天认证失败：") + reason);
}

// 功能：TCP 连接失败或超时后清除请求状态、恢复按钮并显示原因。
void LoginDialog::onConnectionFailed(const QString &reason)
{
    m_pending_request = RequestType::none;
    setRequestButtonsEnabled(true);
    showMessage(QStringLiteral("连接 chat-server 失败：") + reason);
}

// ==================== 模块：界面初始化与信号连接 ====================
// 功能：设置窗口基本属性、输入框提示、密码显示方式和初始提示文本。
void LoginDialog::setupUiState()
{
    setWindowTitle(QStringLiteral("ChatHub 登录"));
    setModal(true);
    ui->pwdEdit->setEchoMode(QLineEdit::Password);
    ui->userNameEdit->setPlaceholderText(QStringLiteral("请输入用户名"));
    ui->pwdEdit->setPlaceholderText(QStringLiteral("请输入密码"));
    ui->messageLabel->clear();
}

// 功能：连接 HTTP 客户端和聊天客户端的结果信号到本对话框对应的处理槽。
void LoginDialog::connectSlots()
{
    connect(m_http, &HttpClient::requestFinish, this, &LoginDialog::onRequestFinish);
    connect(m_http, &HttpClient::requestError, this, &LoginDialog::onRequestError);
    connect(m_http, &HttpClient::requestTimeOut, this, &LoginDialog::onRequestTimeOut);

    connect(m_chat, &ChatClient::authFrameSent, this, &LoginDialog::onAuthFrameSent);
    connect(m_chat, &ChatClient::authSucceeded, this, &LoginDialog::onAuthSucceeded);
    connect(m_chat, &ChatClient::authFailed, this, &LoginDialog::onAuthFailed);
    connect(m_chat, &ChatClient::connectionFailed, this, &LoginDialog::onConnectionFailed);

    // 功能：按钮点击槽由 Qt 设计器的自动连接机制绑定，避免重复发送请求。
}

// ==================== 模块：界面提示辅助 ====================
// 功能：在提示标签中显示当前登录、注册或连接流程的结果。
void LoginDialog::showMessage(const QString &message)
{
    ui->messageLabel->setText(message);
}

// 功能：同步设置登录和注册按钮可用状态，避免请求期间重复提交。
void LoginDialog::setRequestButtonsEnabled(const bool enabled)
{
    ui->loginBtn->setEnabled(enabled);
    ui->registerBtn->setEnabled(enabled);
}
