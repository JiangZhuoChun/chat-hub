# 版本范围 3.5...<3.6：锁定 3.5 LTS，同时拒绝 MSYS2 的 3.6.x（D63/D-004）。
find_package(OpenSSL 3.5...<3.6 REQUIRED)

add_library(chathub_pki STATIC src/dev_ca.cpp)
add_library(chathub::pki ALIAS chathub_pki)

target_compile_features(chathub_pki PUBLIC cxx_std_20)
target_include_directories(chathub_pki
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(chathub_pki
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
)

add_executable(chathub-dev-ca src/dev_ca_main.cpp)
target_link_libraries(chathub-dev-ca PRIVATE chathub::pki)
