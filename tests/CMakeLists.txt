find_package(Qt6 REQUIRED COMPONENTS
        Network
)

# M1-1 合同冒烟测试：验证 contracts target 可被真实消费者链接与运行。
add_executable(chathub_contracts_smoke
        contracts_smoke.cpp
)
target_link_libraries(chathub_contracts_smoke
        PRIVATE
        chathub::contracts
)
add_test(
        NAME chathub.contracts.smoke
        COMMAND chathub_contracts_smoke
)
set_tests_properties(chathub.contracts.smoke PROPERTIES
        LABELS "smoke;contracts"
)

# M1-2 值对象测试：验证公开 include 路径和 C++20 要求会传递给消费者。
add_executable(chathub_contracts_types_test
        contracts_types_test.cpp
)
target_link_libraries(chathub_contracts_types_test
        PRIVATE
        chathub::contracts
)
add_test(
        NAME chathub.contracts.types
        COMMAND chathub_contracts_types_test
)
set_tests_properties(chathub.contracts.types PROPERTIES
        LABELS "unit;contracts"
)

# M1-3 协议注册表测试：验证公开头、编译特性和协议合同共同生效。
add_executable(chathub_contracts_protocol_test
        contracts_protocol_test.cpp
)
target_link_libraries(chathub_contracts_protocol_test
        PRIVATE
        chathub::contracts
)
add_test(
        NAME chathub.contracts.protocol
        COMMAND chathub_contracts_protocol_test
)
set_tests_properties(chathub.contracts.protocol PROPERTIES
        LABELS "unit;contracts"
)

# M1-4 帧层测试：覆盖半包、粘包、边界、非法输入和协议版本。
add_executable(chathub_frame_decoder_test
        frame_decoder_test.cpp
)
target_link_libraries(chathub_frame_decoder_test
        PRIVATE
        chathub::contracts
)
add_test(
        NAME chathub.contracts.frame
        COMMAND chathub_frame_decoder_test
)
set_tests_properties(chathub.contracts.frame PROPERTIES
        LABELS "unit;contracts"
)

# M1-5 开发 CA 测试：覆盖算法、扩展、SAN、序列号、签名和非法 IPv4。
add_executable(chathub_pki_dev_ca_test
        pki_dev_ca_test.cpp
)
target_link_libraries(chathub_pki_dev_ca_test
        PRIVATE
        chathub::pki
)
add_test(
        NAME chathub.pki.dev_ca
        COMMAND chathub_pki_dev_ca_test
)
set_tests_properties(chathub.pki.dev_ca PROPERTIES
        LABELS "unit;pki"
)

# M1-6 Task 1 TLS 上下文测试：覆盖证书加载的成功和失败路径。
add_executable(chathub_tls_context_test
        tls_context_test.cpp
)
target_link_libraries(chathub_tls_context_test
        PRIVATE
        chathub::server::transport
        chathub::pki
)
add_test(
        NAME chathub.server.tls_context
        COMMAND chathub_tls_context_test
)
set_tests_properties(chathub.server.tls_context PROPERTIES
        LABELS "unit;transport"
)


# M1-6 Task 3b：真实 TLS 集成（asio 服务端 handshake ↔ QSslSocket 客户端）。
add_executable(chathub_tls_integration_test tls_integration_test.cpp)
target_link_libraries(chathub_tls_integration_test PRIVATE
        chathub::server::transport
        chathub::client::infrastructure
        chathub::pki
        Qt6::Network
)
# 从 Qt6_DIR 反推 Qt 安装前缀，用于取 TLS 后端插件目录。
get_filename_component(ChatHub_QtPrefix "${Qt6_DIR}/../../.." ABSOLUTE)

# 把 Qt/OpenSSL 运行时 DLL + qopensslbackend.dll 复制到测试 exe 旁。
add_custom_command(TARGET chathub_tls_integration_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:chathub_tls_integration_test>
        $<TARGET_FILE_DIR:chathub_tls_integration_test>
        COMMAND_EXPAND_LISTS
        # Qt 6.5+ TLS 后端是插件；本项目固定使用 OpenSSL 3.5。
        COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:chathub_tls_integration_test>/plugins/tls"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ChatHub_QtPrefix}/plugins/tls/qopensslbackend.dll"
        "$<TARGET_FILE_DIR:chathub_tls_integration_test>/plugins/tls/qopensslbackend.dll"
)
add_test(NAME chathub.tls.integration COMMAND chathub_tls_integration_test)
set_tests_properties(chathub.tls.integration PROPERTIES
        LABELS "integration;tls"
        TIMEOUT 45
)


# M1-7 Task 1 hello 契约测试：锚定连接参数常量与 capability 交集语义。
add_executable(chathub_hello_test
        hello_test.cpp
)
target_link_libraries(chathub_hello_test
        PRIVATE
        chathub::contracts
)
add_test(
        NAME chathub.contracts.hello
        COMMAND chathub_hello_test
)
set_tests_properties(chathub.contracts.hello PROPERTIES
        LABELS "unit;contracts"
)
# M1-7 Task 2 hello codec：双端 JSON codec ↔ 纯值对象映射与往返一致。
add_executable(chathub_hello_codec_test hello_codec_test.cpp)
target_link_libraries(chathub_hello_codec_test PRIVATE
        chathub::server::transport
        chathub::client::infrastructure
        chathub::contracts
)
# QJsonDocument 位于 Qt6::Core，运行仍需 Qt 运行时 DLL。
add_custom_command(TARGET chathub_hello_codec_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:chathub_hello_codec_test>
        $<TARGET_FILE_DIR:chathub_hello_codec_test>
        COMMAND_EXPAND_LISTS
)
add_test(NAME chathub.hello.codec COMMAND chathub_hello_codec_test)
set_tests_properties(chathub.hello.codec PROPERTIES
        LABELS "unit;codec"
)


# 架构边界断言：仅校验各分层 target 的直接链接依赖白名单。
function(chathub_check_direct_links target)
    set(_allowed ${ARGN})
    get_target_property(_deps ${target} INTERFACE_LINK_LIBRARIES)

    if (_deps)
        foreach (_dep IN LISTS _deps)
            # STATIC 库的 PRIVATE 依赖会以 $<LINK_ONLY:...> 回传到 INTERFACE，
            # 它只是链接传递、并非公开接口依赖，不参与架构边界校验（D199/D221）。
            string(FIND "${_dep}" "$<LINK_ONLY:" _link_only_pos)
            if (_link_only_pos EQUAL 0)
                continue()
            endif ()
            if (NOT _dep IN_LIST _allowed)
                message(FATAL_ERROR
                        "架构违规（D199/D221）：${target} 直接链接了白名单之外的 ${_dep}"
                )
            endif ()
        endforeach ()
    endif ()
endfunction()

# 服务端依赖白名单：维持 transport → application → domain ← infrastructure。
chathub_check_direct_links(chathub_server_domain)
chathub_check_direct_links(chathub_server_application chathub::server::domain)
chathub_check_direct_links(chathub_server_infrastructure chathub::server::domain)
chathub_check_direct_links(chathub_server_transport
        chathub::server::application
        asio::asio
        OpenSSL::SSL
        OpenSSL::Crypto
        chathub::contracts
        ws2_32
        mswsock
)

# 客户端依赖白名单：维持 presentation → application → domain ← infrastructure。
chathub_check_direct_links(chathub_client_domain)
chathub_check_direct_links(chathub_client_application chathub::client::domain)
chathub_check_direct_links(chathub_client_infrastructure
        chathub::client::domain
        chathub::contracts
        Qt6::Network
)
chathub_check_direct_links(chathub_client_presentation chathub::client::application)
