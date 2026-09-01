# Qt 客户端 CTest 统一放在此处；公共 Qt/协议依赖由父目录的 helper 提供。

# C1：以本地 TCP 对端验证认证后的历史首屏请求与分块接收。
qt_add_executable(history_client_test
        history_client_test.cpp
        "${CMAKE_CURRENT_LIST_DIR}/../chatclient.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../chatclient.h"
        "${CMAKE_CURRENT_LIST_DIR}/../chat_types.h"
)
configure_qt_client_target(history_client_test)
add_test(NAME history_client_test COMMAND history_client_test)
set_tests_properties(history_client_test PROPERTIES LABELS "qt;network")

# C2.2：验证历史与实时消息合并，以及本地确认后的会话展示顺序。
qt_add_executable(conversation_order_test
        conversation_order_test.cpp
        "${CMAKE_CURRENT_LIST_DIR}/../mainwindow.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../mainwindow.h"
        "${CMAKE_CURRENT_LIST_DIR}/../mainwindow.ui"
        "${CMAKE_CURRENT_LIST_DIR}/../chatclient.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../chatclient.h"
        "${CMAKE_CURRENT_LIST_DIR}/../chat_types.h"
)
configure_qt_client_target(conversation_order_test)
target_link_libraries(conversation_order_test PRIVATE Qt::Widgets)
add_test(NAME conversation_order_test COMMAND conversation_order_test)
set_tests_properties(conversation_order_test PROPERTIES LABELS "qt")
