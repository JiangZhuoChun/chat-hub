# 外部依赖：锁定 OpenSSL 3.5 LTS，同时拒绝 3.6.x（D63/D-004）。
find_package(OpenSSL 3.5...<3.6 REQUIRED)

# PKI 库：封装开发 CA 的证书和私钥生成能力。
add_library(chathub_pki STATIC
    src/dev_ca.cpp
)
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

# 命令行工具：复用 PKI 库生成本地开发证书。
add_executable(chathub-dev-ca
    src/dev_ca_main.cpp
)
target_link_libraries(chathub-dev-ca
    PRIVATE
        chathub::pki
)
