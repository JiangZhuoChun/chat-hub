#----------CTest----------
add_executable(chathub_contracts_smoke contracts_smoke.cpp)
target_link_libraries(chathub_contracts_smoke PRIVATE
        chathub::contracts
)
add_test(NAME chathub.contracts.smoke COMMAND chathub_contracts_smoke)
set_tests_properties(chathub.contracts.smoke PROPERTIES LABELS "smoke;contracts")