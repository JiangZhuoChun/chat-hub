#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QUrl>

class HttpClient : public QObject
{
    Q_OBJECT

  public:
    // ==================== 模块：生命周期与请求配置 ====================
    // 功能：创建 HTTP 客户端，并由父对象管理其 Qt 对象生命周期。
    explicit HttpClient(QObject *parent = nullptr);

    // 功能：设置每个 HTTP 请求等待响应的最长时间，单位为毫秒。
    void setTimeOut(unsigned int ms);

    // ==================== 模块：HTTP 请求发送 ====================
    // 功能：向指定地址发送 GET 请求，并将响应交给统一的响应对象生命周期管理。
    void get(const QUrl &url);

    // 功能：向指定地址发送 JSON 格式的 POST 请求，并将响应交给统一的响应对象生命周期管理。
    void post(const QUrl &url, const QByteArray &body);

  signals:
    // ==================== 模块：HTTP 请求结果通知 ====================
    // 功能：在请求超过设定等待时间且尚未完成时通知界面。
    void requestTimeOut();

    // 功能：在收到任意 HTTP 状态码响应时通知状态码和响应正文。
    void requestFinish(int status_code, const QByteArray &body);

    // 功能：在未获得 HTTP 响应的底层网络错误发生时通知错误原因。
    void requestError(const QString &error);

  private:
    // ==================== 模块：Reply 生命周期管理 ====================
    // 功能：为响应对象绑定超时、完成、错误转发和延迟释放处理。
    void setupReply(QNetworkReply *reply);

    // ==================== 模块：网络资源 ====================
    // 功能：创建并管理所有 HTTP 请求对应的 QNetworkReply 对象。
    QNetworkAccessManager m_manager;

    // ==================== 模块：请求配置状态 ====================
    // 功能：保存新建请求的超时时间，单位为毫秒。
    unsigned int m_timeout_ms = 5000;
};
