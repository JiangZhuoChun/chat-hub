#include "HttpClient.h"

#include <QNetworkRequest>
#include <QTimer>

// ==================== 模块：生命周期与请求配置 ====================
// 功能：创建 HTTP 客户端，初始化由类成员负责管理的网络资源。
HttpClient::HttpClient(QObject *parent) : QObject(parent)
{
}

// 功能：更新后续 HTTP 请求使用的超时时间，单位为毫秒。
void HttpClient::setTimeOut(const unsigned int ms)
{
    m_timeout_ms = ms;
}

// ==================== 模块：HTTP 请求发送 ====================
// 功能：发送 GET 请求，并将返回的响应对象交给统一生命周期管理。
void HttpClient::get(const QUrl &url)
{
    QNetworkReply *reply = m_manager.get(QNetworkRequest(url));
    setupReply(reply);
}

// 功能：发送 JSON 格式的 POST 请求，并将响应对象交给统一生命周期管理。
void HttpClient::post(const QUrl &url, const QByteArray &body)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_manager.post(request, body);
    setupReply(reply);
}

// ==================== 模块：Reply 生命周期管理 ====================
// 功能：为单个响应对象创建超时计时器，并统一转发完成结果和底层网络错误。
void HttpClient::setupReply(QNetworkReply *reply)
{
    QTimer *timer = new QTimer(reply);
    timer->setSingleShot(true);
    timer->start(m_timeout_ms);

    connect(timer, &QTimer::timeout, this,
            // 功能：在响应对象尚未结束时中止请求并发出超时信号。
            [this, reply] {
                if (reply->isFinished())
                {
                    return;
                }
                reply->setProperty("TimeOut", true);
                reply->abort();
                emit requestTimeOut();
            });

    connect(reply, &QNetworkReply::finished, this,
            // 功能：停止计时、区分超时和网络错误，并在处理后释放响应对象。
            [this, reply, timer] {
                timer->stop();
                if (reply->property("TimeOut").toBool())
                {
                    reply->deleteLater();
                    return;
                }

                const auto status_code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status_code > 0)
                {
                    emit requestFinish(status_code, reply->readAll());
                }
                else if (reply->error() != QNetworkReply::NoError)
                {
                    emit requestError(reply->errorString());
                }

                reply->deleteLater();
            });
}
