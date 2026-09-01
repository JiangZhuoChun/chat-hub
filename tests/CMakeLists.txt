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

#----------架构边界断言（最小版：直接链接白名单）----------
function(chathub_check_direct_links target)
    set(_allowed ${ARGN})
    get_target_property(_deps ${target} INTERFACE_LINK_LIBRARIES)
    if(_deps)
        foreach(_dep IN LISTS _deps)
            if(NOT _dep IN_LIST _allowed)
                message(FATAL_ERROR
                        "架构违规（D199/D221）：${target} 直接链接了白名单之外的 ${_dep}")
            endif()
        endforeach()
    endif()
endfunction()

chathub_check_direct_links(chathub_server_domain)
chathub_check_direct_links(chathub_server_application chathub::server::domain)
chathub_check_direct_links(chathub_server_infrastructure chathub::server::domain)
chathub_check_direct_links(chathub_server_transport chathub::server::application)
chathub_check_direct_links(chathub_client_domain)
chathub_check_direct_links(chathub_client_application chathub::client::domain)
chathub_check_direct_links(chathub_client_infrastructure chathub::client::domain)
chathub_check_direct_links(chathub_client_presentation chathub::client::application)
