#----------CTest----------
add_executable(chathub_contracts_smoke contracts_smoke.cpp)
target_link_libraries(chathub_contracts_smoke PRIVATE
        chathub::contracts
)
add_test(NAME chathub.contracts.smoke COMMAND chathub_contracts_smoke)
set_tests_properties(chathub.contracts.smoke PROPERTIES LABELS "smoke;contracts")

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