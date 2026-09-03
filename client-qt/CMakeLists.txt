# 客户端分层：presentation → application → domain ← infrastructure

# Domain 层：纯 C++20 客户端业务模型，不依赖 Qt。
add_library(chathub_client_domain INTERFACE)
add_library(chathub::client::domain ALIAS chathub_client_domain)
target_compile_features(chathub_client_domain INTERFACE cxx_std_20)

# Application 层：编排用例，只依赖 Domain 层公开合同。
add_library(chathub_client_application INTERFACE)
add_library(chathub::client::application ALIAS chathub_client_application)
target_link_libraries(chathub_client_application INTERFACE
        chathub::client::domain
)

# Infrastructure 层：真实 Qt 网络 adapter 使用 STATIC 库和独立编译单元。
qt_add_library(chathub_client_infrastructure STATIC
        src/tls_client.cpp
        src/hello_codec.cpp
        include/chathub/client/infrastructure/tls_client.hpp
        include/chathub/client/infrastructure/hello_codec.hpp
)
add_library(chathub::client::infrastructure ALIAS chathub_client_infrastructure)
target_compile_features(chathub_client_infrastructure PUBLIC cxx_std_20)
target_include_directories(chathub_client_infrastructure PUBLIC include)

# Qt Network 类型出现在公开头中，因此作为 PUBLIC 使用要求传递给消费者。
target_link_libraries(chathub_client_infrastructure PUBLIC
        chathub::client::domain
        chathub::contracts
        Qt6::Network
)

# Presentation 层：界面展示只依赖 Application 层。
add_library(chathub_client_presentation INTERFACE)
add_library(chathub::client::presentation ALIAS chathub_client_presentation)
target_link_libraries(chathub_client_presentation INTERFACE
        chathub::client::application
)
