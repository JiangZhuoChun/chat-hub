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

# 架构边界断言：仅校验各分层 target 的直接链接依赖白名单。
function(chathub_check_direct_links target)
    set(_allowed ${ARGN})
    get_target_property(_deps ${target} INTERFACE_LINK_LIBRARIES)

    if (_deps)
        foreach (_dep IN LISTS _deps)
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
    Qt6::Network
)
chathub_check_direct_links(chathub_client_presentation chathub::client::application)
