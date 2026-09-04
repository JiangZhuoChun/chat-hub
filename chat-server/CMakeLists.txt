# 服务端分层：transport → application → domain ← infrastructure

# 外部依赖：锁定 standalone Asio 与 OpenSSL 3.5 LTS。
find_package(asio CONFIG REQUIRED)
find_package(OpenSSL 3.5...<3.6 REQUIRED)
find_package(Boost REQUIRED COMPONENTS json)

# Domain 层：纯 C++20 业务模型，不依赖基础设施。
add_library(chathub_server_domain INTERFACE)
add_library(chathub::server::domain ALIAS chathub_server_domain)
target_compile_features(chathub_server_domain INTERFACE cxx_std_20)

# Application 层：只依赖 Domain 层公开合同。
add_library(chathub_server_application INTERFACE)
add_library(chathub::server::application ALIAS chathub_server_application)
target_link_libraries(chathub_server_application INTERFACE
        chathub::server::domain
)

# Infrastructure 层：基础设施 adapter 只能向内依赖 Domain。
add_library(chathub_server_infrastructure INTERFACE)
add_library(chathub::server::infrastructure ALIAS chathub_server_infrastructure)
target_link_libraries(chathub_server_infrastructure INTERFACE
        chathub::server::domain
)

# Transport 层：真实网络 adapter 使用 STATIC 库和独立编译单元。
add_library(chathub_server_transport STATIC
        src/tls_context.cpp
        src/tls_handshake.cpp
        src/hello_codec.cpp
        include/chathub/server/transport/tls_context.hpp
        include/chathub/server/transport/tls_handshake.hpp
        include/chathub/server/transport/hello_codec.hpp
)
add_library(chathub::server::transport ALIAS chathub_server_transport)
target_compile_features(chathub_server_transport PUBLIC cxx_std_20)
target_include_directories(chathub_server_transport PUBLIC include)

# Windows 编译合同：Asio 使用 Windows 10 API 下限。
if (WIN32)
    target_compile_definitions(chathub_server_transport PUBLIC
            _WIN32_WINNT=0x0A00
    )
endif ()

# Transport 公开依赖会传递给实际网络会话和测试消费者。
target_link_libraries(chathub_server_transport PUBLIC
        chathub::server::application
        asio::asio
        OpenSSL::SSL
        OpenSSL::Crypto
        chathub::contracts
)
# Boost.JSON 只出现在 hello_codec.cpp（实现），不进公开头，不传播给消费者。
target_link_libraries(chathub_server_transport PRIVATE
        Boost::json
)

# Windows 系统库：standalone Asio 的消费者必须链接 Winsock。
if (WIN32)
    target_link_libraries(chathub_server_transport PUBLIC
            ws2_32
            mswsock
    )
endif ()
