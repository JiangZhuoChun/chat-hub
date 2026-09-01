#pragma once

#include <QByteArray>
#include <QDialog>
#include <QString>

class ChatClient;
class HttpClient;

namespace Ui
{
class LoginDialog;
}

class LoginDialog : public QDialog
{
    Q_OBJECT

  public:
    // ==================== 模块：生命周期与登录结果 ====================
    // 功能：创建登录对话框，并注入已创建的聊天客户端以完成 TCP 认证。
    explicit LoginDialog(ChatClient *chat_client, QWidget *parent = nullptr);
    // 功能：释放登录界面对象。
    ~LoginDialog() override;
    // 功能：返回最近一次登录成功后保存的用户名。
    QString username() const;

  protected:
    // ==================== 模块：对话框关闭处理 ====================
    // 功能：取消对话框时主动断开聊天连接，再执行父类的拒绝操作。
    void reject() override;

  private slots:
    // ==================== 模块：用户界面操作 ====================
    // 功能：校验输入并向登录接口提交用户名、密码。
    void on_loginBtn_clicked();
    // 功能：校验输入并向注册接口提交用户名、密码。
    void on_registerBtn_clicked();
    // 功能：处理取消按钮，关闭登录对话框。
    void on_cancelBtn_clicked();

    // ==================== 模块：HTTP 请求结果处理 ====================
    // 功能：解析登录或注册响应；登录成功后将令牌交给 ChatClient 连接服务器。
    void onRequestFinish(int status_code, const QByteArray &body);
    // 功能：处理没有收到 HTTP 响应的底层网络错误。
    void onRequestError(const QString &error);
    // 功能：处理 HTTP 请求超过等待时间的情况。
    void onRequestTimeOut();

    // ==================== 模块：TCP 认证结果处理 ====================
    // 功能：在认证帧写入发送缓冲区后更新界面提示。
    void onAuthFrameSent();
    // 功能：认证成功后恢复按钮、显示结果并关闭对话框。
    void onAuthSucceeded();
    // 功能：认证被拒绝或认证协议异常时恢复按钮并显示原因。
    void onAuthFailed(const QString &reason);
    // 功能：TCP 连接未建立或连接超时时恢复按钮并显示原因。
    void onConnectionFailed(const QString &reason);

  private:
    // ==================== 模块：请求状态类型 ====================
    // 功能：区分当前等待的是登录响应、注册响应，还是没有待处理请求。
    enum class RequestType
    {
        none,
        login,
        registerUser
    };

    // ==================== 模块：界面初始化与信号连接 ====================
    // 功能：设置对话框标题、输入框提示、密码回显方式和初始提示文本。
    void setupUiState();
    // 功能：连接 HTTP 客户端和聊天客户端的结果信号到本对话框的处理槽。
    void connectSlots();

    // ==================== 模块：界面提示辅助 ====================
    // 功能：在对话框底部显示当前登录、注册或连接流程提示。
    void showMessage(const QString &message);
    // 功能：同时启用或禁用登录、注册按钮，避免重复提交请求。
    void setRequestButtonsEnabled(bool enabled);

    // ==================== 模块：界面与外部依赖 ====================
    // 界面对象，析构时显式释放。
    Ui::LoginDialog *ui;
    // 当前对话框创建并通过 Qt 父子关系管理。
    HttpClient *m_http;
    // 外部注入、仅借用。
    ChatClient *m_chat;

    // ==================== 模块：登录流程状态和登录结果 ====================
    // 功能：记录正在等待哪一种 HTTP 请求的结果----登录状态
    RequestType m_pending_request{RequestType::none};
    // 功能：保存 TCP 认证成功后提供给主窗口使用的用户名。----登录结果状态
    QString m_username;
};
