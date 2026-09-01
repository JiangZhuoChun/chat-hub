# 生产 ChatServer：这里仅声明运行程序和它直接使用的持久化/加密依赖。
add_executable(chat-server
        main.cpp
        "${CHATHUB_CHAT_SERVER_ROOT}/src/net/server.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/net/session.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/protocol/frame_decoder.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/protocol/chat_payload.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/sqlite_message_repository.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_connection_config.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_connection.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_message_repository.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/auth/asio_auth_introspection_client.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/auth/http_response_parser.cpp"
        server_runtime_config.cpp
        message_repository_factory.cpp
        auth_introspection_config.cpp
)
configure_chat_server_target(chat-server)
target_link_libraries(chat-server PRIVATE
        OpenSSL::Crypto
        SQLite3::SQLite3
        unofficial::libmariadb
)
link_chat_server_json(chat-server)
link_chat_server_jwt(chat-server)
link_chat_server_network(chat-server)

# MySQL 8 的 caching_sha2_password 认证需要 libmariadb 的插件 DLL。
# 构建服务端后复制到可执行程序旁边，不把 vcpkg 安装目录当作运行目录。
if (NOT DEFINED VCPKG_INSTALLED_DIR OR NOT DEFINED VCPKG_TARGET_TRIPLET)
    message(FATAL_ERROR "vcpkg manifest variables are required")
endif ()

set(mariadb_plugin_source_dir
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/plugins/libmariadb")
if (NOT EXISTS "${mariadb_plugin_source_dir}/caching_sha2_password.dll")
    message(FATAL_ERROR "MariaDB caching_sha2_password plugin is missing")
endif ()

add_custom_command(
        TARGET chat-server
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:chat-server>/plugins/libmariadb"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${mariadb_plugin_source_dir}"
        "$<TARGET_FILE_DIR:chat-server>/plugins/libmariadb"
        VERBATIM
)
