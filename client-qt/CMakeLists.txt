# 客户端分层：presentation → application → domain ← infrastructure
find_package(Qt6 REQUIRED COMPONENTS Core Network)

add_library(chathub_client_domain INTERFACE)
add_library(chathub::client::domain ALIAS chathub_client_domain)
target_compile_features(chathub_client_domain INTERFACE cxx_std_20)

add_library(chathub_client_application INTERFACE)
add_library(chathub::client::application ALIAS chathub_client_application)
target_link_libraries(chathub_client_application INTERFACE
        chathub::client::domain
)

# infrastructure 是真实网络 adapter：STATIC + .cpp，给 clangd 一个编译单元，
# 使 Qt 头（QList 等）能被正确解析（header-only INTERFACE 会让 IDE 找不到 Qt include）。
add_library(chathub_client_infrastructure STATIC
        src/tls_client.cpp
)
add_library(chathub::client::infrastructure ALIAS chathub_client_infrastructure)
target_compile_features(chathub_client_infrastructure PUBLIC cxx_std_20)
target_include_directories(chathub_client_infrastructure PUBLIC include)
target_link_libraries(chathub_client_infrastructure PUBLIC
        chathub::client::domain
        Qt6::Network
)
target_sources(chathub_client_infrastructure PRIVATE
        include/chathub/client/infrastructure/tls_client.hpp
)

add_library(chathub_client_presentation INTERFACE)
add_library(chathub::client::presentation ALIAS chathub_client_presentation)
target_link_libraries(chathub_client_presentation INTERFACE
        chathub::client::application
)