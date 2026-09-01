# 本文件仅登记 ChatServer 的 CTest 目标和测试属性。
# 生产程序位于 ../app，JWT 演示位于 ../examples，避免三类职责混在同一清单。

# ---- 无外部 MySQL 服务的单元与本机网络测试 ----
add_executable(frame_decoder_test
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/frame_decoder_test.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/protocol/frame_decoder.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/protocol/chat_payload.cpp"
)
configure_chat_server_target(frame_decoder_test)
link_chat_server_json(frame_decoder_test)
link_chat_server_network(frame_decoder_test)
register_chat_server_test(frame_decoder_test)

add_executable(message_repository_test
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/message_repository_test.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/message_repository_contract_test_runner.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/sqlite_message_repository.cpp"
)
configure_chat_server_target(message_repository_test)
target_link_libraries(message_repository_test PRIVATE SQLite3::SQLite3)
register_chat_server_test(message_repository_test)

add_executable(history_response_test
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/history_response_test.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/net/session.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/protocol/frame_decoder.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/protocol/chat_payload.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/sqlite_message_repository.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/auth/asio_auth_introspection_client.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/auth/http_response_parser.cpp"
)
configure_chat_server_target(history_response_test)
target_link_libraries(history_response_test PRIVATE OpenSSL::Crypto SQLite3::SQLite3)
link_chat_server_json(history_response_test)
link_chat_server_jwt(history_response_test)
link_chat_server_network(history_response_test)
register_chat_server_test(history_response_test)

add_executable(http_response_parser_test
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/http_response_parser_test.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/auth/http_response_parser.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/protocol/chat_payload.cpp"
)
configure_chat_server_target(http_response_parser_test)
link_chat_server_json(http_response_parser_test)
register_chat_server_test(http_response_parser_test)

add_executable(auth_introspection_client_test
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/auth_introspection_client_test.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/auth/asio_auth_introspection_client.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/auth/http_response_parser.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/protocol/chat_payload.cpp"
)
configure_chat_server_target(auth_introspection_client_test)
link_chat_server_json(auth_introspection_client_test)
link_chat_server_network(auth_introspection_client_test)
register_chat_server_test(auth_introspection_client_test)

add_executable(auth_introspection_config_test
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/auth_introspection_config_test.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/app/auth_introspection_config.cpp"
)
configure_chat_server_target(auth_introspection_config_test)
target_include_directories(auth_introspection_config_test PRIVATE
        "${CHATHUB_CHAT_SERVER_ROOT}/app"
)
register_chat_server_test(auth_introspection_config_test)

add_executable(server_runtime_config_test
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/server_runtime_config_test.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/app/server_runtime_config.cpp"
)
configure_chat_server_target(server_runtime_config_test)
target_include_directories(server_runtime_config_test PRIVATE
        "${CHATHUB_CHAT_SERVER_ROOT}/app"
)
register_chat_server_test(server_runtime_config_test)

add_executable(online_users_integration_test
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/online_users_integration_test.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/net/server.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/net/session.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/protocol/frame_decoder.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/protocol/chat_payload.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/sqlite_message_repository.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/auth/asio_auth_introspection_client.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/auth/http_response_parser.cpp"
)
configure_chat_server_target(online_users_integration_test)
target_link_libraries(online_users_integration_test PRIVATE OpenSSL::Crypto SQLite3::SQLite3)
link_chat_server_json(online_users_integration_test)
link_chat_server_jwt(online_users_integration_test)
link_chat_server_network(online_users_integration_test)
register_chat_server_test(online_users_integration_test)

# ---- 启动真实 chat-server 子进程的配置/集成测试 ----
add_executable(server_process_config_test
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/server_process_config_test.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/protocol/frame_decoder.cpp"
)
configure_chat_server_target(server_process_config_test)
link_chat_server_json(server_process_config_test)
link_chat_server_network(server_process_config_test)
add_dependencies(server_process_config_test chat-server)
add_test(NAME server_process_config_test
        COMMAND $<TARGET_FILE:server_process_config_test> $<TARGET_FILE:chat-server>
)

foreach(mysql_process_case IN ITEMS startup chat history schema-failure)
    string(REPLACE "-" "_" mysql_process_test_suffix "${mysql_process_case}")
    add_test(NAME "mysql_server_process_${mysql_process_test_suffix}_test"
            COMMAND $<TARGET_FILE:server_process_config_test>
            $<TARGET_FILE:chat-server>
            "--mysql-${mysql_process_case}"
    )
    set_tests_properties("mysql_server_process_${mysql_process_test_suffix}_test" PROPERTIES
            SKIP_RETURN_CODE 77
            RUN_SERIAL TRUE
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endforeach ()

# ---- 需要真实 MySQL 环境的 Repository 专项测试 ----
function(add_mysql_repository_test target_name)
    add_dependencies(${target_name} chat-server)
    set_tests_properties(${target_name} PROPERTIES
            SKIP_RETURN_CODE 77
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endfunction()

add_executable(mysql_connection_test
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/mysql_connection_test.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_connection_config.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_connection.cpp"
)
configure_chat_server_target(mysql_connection_test)
target_link_libraries(mysql_connection_test PRIVATE unofficial::libmariadb)
# 先构建 chat-server，保证其 POST_BUILD 插件部署已执行。
add_dependencies(mysql_connection_test chat-server)
add_test(NAME mysql_connection_test
        COMMAND $<TARGET_FILE:mysql_connection_test>
        "$<TARGET_FILE_DIR:chat-server>/plugins/libmariadb"
)
set_tests_properties(mysql_connection_test PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_executable(mysql_connection_integration_test
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/mysql_connection_integration_test.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_connection_config.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_connection.cpp"
)
configure_chat_server_target(mysql_connection_integration_test)
target_link_libraries(mysql_connection_integration_test PRIVATE unofficial::libmariadb)
add_test(NAME mysql_connection_integration_test COMMAND $<TARGET_FILE:mysql_connection_integration_test>)
add_mysql_repository_test(mysql_connection_integration_test)

add_executable(mysql_message_repository_connection_test
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/mysql_message_repository_connection_test.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_connection_config.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_connection.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_message_repository.cpp"
)
configure_chat_server_target(mysql_message_repository_connection_test)
target_link_libraries(mysql_message_repository_connection_test PRIVATE unofficial::libmariadb)
add_test(NAME mysql_message_repository_connection_test
        COMMAND $<TARGET_FILE:mysql_message_repository_connection_test>)
add_mysql_repository_test(mysql_message_repository_connection_test)

add_executable(mysql_message_repository_declaration_test
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/mysql_message_repository_declaration_test.cpp"
)
configure_chat_server_target(mysql_message_repository_declaration_test)
register_chat_server_test(mysql_message_repository_declaration_test)

add_executable(mysql_message_repository_schema_test
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/mysql_message_repository_schema_test.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_connection_config.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_connection.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_message_repository.cpp"
)
configure_chat_server_target(mysql_message_repository_schema_test)
target_link_libraries(mysql_message_repository_schema_test PRIVATE unofficial::libmariadb)
add_test(NAME mysql_message_repository_schema_test
        COMMAND $<TARGET_FILE:mysql_message_repository_schema_test>)
add_mysql_repository_test(mysql_message_repository_schema_test)
set_tests_properties(mysql_message_repository_schema_test PROPERTIES RUN_SERIAL TRUE)

add_executable(mysql_message_repository_store_test
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/mysql_message_repository_store_test.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_connection_config.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_connection.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_message_repository.cpp"
)
configure_chat_server_target(mysql_message_repository_store_test)
target_link_libraries(mysql_message_repository_store_test PRIVATE
        unofficial::libmariadb
        Threads::Threads
)
add_test(NAME mysql_message_repository_store_test
        COMMAND $<TARGET_FILE:mysql_message_repository_store_test>)
add_mysql_repository_test(mysql_message_repository_store_test)
set_tests_properties(mysql_message_repository_store_test PROPERTIES RUN_SERIAL TRUE)

add_executable(mysql_message_repository_history_test
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/mysql_message_repository_history_test.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_connection_config.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_connection.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_message_repository.cpp"
)
configure_chat_server_target(mysql_message_repository_history_test)
target_link_libraries(mysql_message_repository_history_test PRIVATE unofficial::libmariadb)
add_test(NAME mysql_message_repository_history_test
        COMMAND $<TARGET_FILE:mysql_message_repository_history_test>)
add_mysql_repository_test(mysql_message_repository_history_test)
set_tests_properties(mysql_message_repository_history_test PROPERTIES RUN_SERIAL TRUE)

add_executable(mysql_message_repository_contract_test
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/mysql_message_repository_contract_test.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/examples/message_repository_contract_test_runner.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_connection_config.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_connection.cpp"
        "${CHATHUB_CHAT_SERVER_ROOT}/src/repository/mysql_message_repository.cpp"
)
configure_chat_server_target(mysql_message_repository_contract_test)
target_link_libraries(mysql_message_repository_contract_test PRIVATE unofficial::libmariadb)
add_test(NAME mysql_message_repository_contract_test
        COMMAND $<TARGET_FILE:mysql_message_repository_contract_test>)
add_mysql_repository_test(mysql_message_repository_contract_test)
set_tests_properties(mysql_message_repository_contract_test PROPERTIES RUN_SERIAL TRUE)

# W13-2：标签描述实际运行依赖，不改变 PASS / FAIL / SKIP 语义。
set_tests_properties(
        frame_decoder_test
        message_repository_test
        history_response_test
        http_response_parser_test
        auth_introspection_config_test
        server_runtime_config_test
        mysql_connection_test
        mysql_message_repository_declaration_test
        PROPERTIES LABELS "unit"
)
set_tests_properties(
        auth_introspection_client_test
        online_users_integration_test
        PROPERTIES LABELS "network"
)
set_tests_properties(server_process_config_test PROPERTIES LABELS "process")
set_tests_properties(
        mysql_server_process_startup_test
        mysql_server_process_chat_test
        mysql_server_process_history_test
        mysql_server_process_schema_failure_test
        PROPERTIES LABELS "process;mysql"
)
set_tests_properties(
        mysql_connection_integration_test
        mysql_message_repository_connection_test
        mysql_message_repository_schema_test
        mysql_message_repository_store_test
        mysql_message_repository_history_test
        mysql_message_repository_contract_test
        PROPERTIES LABELS "mysql"
)
