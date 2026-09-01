#include "chatclient.h"
#include "logindialog.h"
#include "mainwindow.h"

#include <QApplication>

// ==================== 模块：Qt 客户端启动入口 ====================
// 功能：先运行登录对话框，认证成功后创建主聊天窗口并进入 Qt 事件循环。
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    ChatClient chat_client;

    QString username;
    {
        LoginDialog login_dialog(&chat_client);
        if (login_dialog.exec() != QDialog::Accepted)
        {
            return 0;
        }
        username = login_dialog.username();
    }

    MainWindow chat_window(&chat_client, username);
    chat_window.show();
    return app.exec();
}
