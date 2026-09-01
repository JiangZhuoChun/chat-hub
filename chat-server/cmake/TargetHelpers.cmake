# 每个 ChatServer 可执行目标先调用此函数，保证使用相同的项目头文件、
# ASIO 宏和 vcpkg 导入目标。业务库（SQLite/MySQL）仍由实际使用者显式链接。
function(configure_chat_server_target target_name)
    target_link_libraries(${target_name} PRIVATE chathub_chat_server_options)

    # CMake 文件按职责分目录，但所有服务端可执行程序保持原有同一运行目录。
    # 进程测试和 MySQL 插件路径由可执行程序位置推导，因此不能随声明目录漂移。
    set_target_properties(${target_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/chat-server"
    )
endfunction()

# 默认测试运行目标自身。需要额外命令行参数的进程级测试可直接调用 add_test。
function(register_chat_server_test target_name)
    add_test(NAME ${target_name} COMMAND $<TARGET_FILE:${target_name}>)
endfunction()

# 只有编译了 boost::json 相关正文或 HTTP 代码的目标才链接预编译 Boost.JSON。
function(link_chat_server_json target_name)
    target_link_libraries(${target_name} PRIVATE Boost::json)
endfunction()

# jwt-cpp 的 picojson feature 由 vcpkg manifest 声明；此函数只分配给 JWT 使用者。
function(link_chat_server_jwt target_name)
    target_link_libraries(${target_name} PRIVATE jwt-cpp::jwt-cpp)
endfunction()

# 当前网络实现使用 Windows Winsock 扩展。非 Windows 配置不会引入 Windows 库。
function(link_chat_server_network target_name)
    if (WIN32)
        target_link_libraries(${target_name} PRIVATE ws2_32 mswsock)
    endif ()
endfunction()
