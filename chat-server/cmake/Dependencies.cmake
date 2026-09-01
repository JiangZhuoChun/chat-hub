# 本文件是 ChatServer 对第三方依赖的唯一入口。
# 所有目标通过 chathub_chat_server_options 获取公共头文件、编译宏和库，
# 从而不再依赖开发机上的绝对 include/lib 路径。

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS json)
find_package(jwt-cpp CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(unofficial-libmariadb CONFIG REQUIRED)

# 项目历史代码使用 SQLite3::SQLite3。vcpkg 的 sqlite3 包导出
# unofficial::sqlite3::sqlite3；别名保留源码目标的命名稳定性，避免改动业务代码。
if (NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS unofficial::sqlite3::sqlite3)
endif ()

# INTERFACE 库不产生二进制，只集中传播每个服务端目标都需要的使用要求。
# Asio 是头文件库；Boost.JSON、JWT 和 Windows socket 库由实际使用它们的目标显式链接。
add_library(chathub_chat_server_options INTERFACE)
target_include_directories(chathub_chat_server_options INTERFACE
        "${CHATHUB_CHAT_SERVER_ROOT}/include"
)
target_compile_definitions(chathub_chat_server_options INTERFACE
        ASIO_STANDALONE
        _WIN32_WINNT=0x0A00
)
target_link_libraries(chathub_chat_server_options INTERFACE
        asio::asio
)
