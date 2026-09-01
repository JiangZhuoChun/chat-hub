cmake_minimum_required(VERSION 3.19)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network)
qt_standard_project_setup(REQUIRES 6.8)

# Qt 客户端的公共依赖只定义一次；应用和测试通过 helper 复用它。
add_library(chathub_qt_client_options INTERFACE)
target_include_directories(chathub_qt_client_options INTERFACE
        "${CMAKE_SOURCE_DIR}/chat-server/include"
)
target_link_libraries(chathub_qt_client_options INTERFACE
        Qt::Core
        Qt6::Network
)

function(configure_qt_client_target target_name)
    target_link_libraries(${target_name} PRIVATE chathub_qt_client_options)
endfunction()

qt_add_executable(client-qt
        WIN32 MACOSX_BUNDLE
        main.cpp
        mainwindow.cpp
        mainwindow.h
        HttpClient.h
        HttpClient.cpp
        mainwindow.ui
        logindialog.cpp
        logindialog.h
        logindialog.ui
        chatclient.cpp
        chatclient.h
        chat_types.h
)
configure_qt_client_target(client-qt)

target_link_libraries(client-qt
        PRIVATE
        Qt::Widgets
)

install(TARGETS client-qt
        BUNDLE DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
        TARGET client-qt
        OUTPUT_SCRIPT deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

# 测试和应用配置分离：在查看客户端入口时，不需要浏览 CTest 细节。
add_subdirectory(tests)
