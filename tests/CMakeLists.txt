# CTest 的最小构建闭环：验证 contracts target 可被真实消费者链接与运行。
add_executable(chathub_contracts_smoke contracts_smoke.cpp)
target_link_libraries(chathub_contracts_smoke PRIVATE
        chathub::contracts
)
add_test(NAME chathub.contracts.smoke COMMAND chathub_contracts_smoke)
set_tests_properties(chathub.contracts.smoke PROPERTIES LABELS "smoke;contracts")

# M1-2 的值对象测试。PRIVATE 链接同时验证公开 include 目录和 C++20 编译特性会传递给消费者。
add_executable(chathub_contracts_types_test contracts_types_test.cpp)
target_link_libraries(chathub_contracts_types_test PRIVATE
        chathub::contracts
)
add_test(NAME chathub.contracts.types COMMAND chathub_contracts_types_test)
# 标签让后续可按 unit 或 contracts 维度筛选测试。
set_tests_properties(chathub.contracts.types PROPERTIES LABELS "unit;contracts")

# M1-3 注册表测试：作为 contracts 的真实消费者，验证公开头、C++20 特性和协议合同一起生效。
add_executable(chathub_contracts_protocol_test contracts_protocol_test.cpp)
target_link_libraries(chathub_contracts_protocol_test PRIVATE
        chathub::contracts
)
add_test(NAME chathub.contracts.protocol COMMAND chathub_contracts_protocol_test)
set_tests_properties(chathub.contracts.protocol PROPERTIES LABELS "unit;contracts")

#M1-4 帧层单元测试：半包、粘包、空正文、超长、非法 UTF-8、未知 type、版本不兼容、往返。
add_executable(chathub_frame_decoder_test frame_decoder_test.cpp)
target_link_libraries(chathub_frame_decoder_test PRIVATE
        chathub::contracts
)
add_test(NAME chathub.contracts.frame COMMAND chathub_frame_decoder_test)
set_tests_properties(chathub.contracts.frame PROPERTIES LABELS "unit;contracts")


# M1-5 开发 CA：算法、扩展、SAN、序列号、链签名、非法 IPv4。
add_executable(chathub_pki_dev_ca_test pki_dev_ca_test.cpp)
target_link_libraries(chathub_pki_dev_ca_test PRIVATE
        chathub::pki
)
add_test(NAME chathub.pki.dev_ca COMMAND chathub_pki_dev_ca_test)
set_tests_properties(chathub.pki.dev_ca PROPERTIES LABELS "unit;pki")

#----------架构边界断言（最小版：直接链接白名单）----------
function(chathub_check_direct_links target)
    set(_allowed ${ARGN})
    get_target_property(_deps ${target} INTERFACE_LINK_LIBRARIES)
    if (_deps)
        foreach (_dep IN LISTS _deps)
            if (NOT _dep IN_LIST _allowed)
                message(FATAL_ERROR
                        "架构违规（D199/D221）：${target} 直接链接了白名单之外的 ${_dep}")
            endif ()
        endforeach ()
    endif ()
endfunction()

chathub_check_direct_links(chathub_server_domain)
chathub_check_direct_links(chathub_server_application chathub::server::domain)
chathub_check_direct_links(chathub_server_infrastructure chathub::server::domain)
chathub_check_direct_links(chathub_server_transport chathub::server::application)
chathub_check_direct_links(chathub_client_domain)
chathub_check_direct_links(chathub_client_application chathub::client::domain)
chathub_check_direct_links(chathub_client_infrastructure chathub::client::domain)
chathub_check_direct_links(chathub_client_presentation chathub::client::application)
