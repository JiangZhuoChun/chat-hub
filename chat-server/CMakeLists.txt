# 服务端分层：transport → application → domain ← infrastructure
find_package(asio CONFIG REQUIRED)
find_package(OpenSSL 3.5...<3.6 REQUIRED)

add_library(chathub_server_domain INTERFACE)
add_library(chathub::server::domain ALIAS chathub_server_domain)
target_compile_features(chathub_server_domain INTERFACE cxx_std_20)

add_library(chathub_server_application INTERFACE)
add_library(chathub::server::application ALIAS chathub_server_application)
target_link_libraries(chathub_server_application INTERFACE
        chathub::server::domain
)

add_library(chathub_server_infrastructure INTERFACE)
add_library(chathub::server::infrastructure ALIAS chathub_server_infrastructure)
target_link_libraries(chathub_server_infrastructure INTERFACE
        chathub::server::domain
)

add_library(chathub_server_transport INTERFACE)
add_library(chathub::server::transport ALIAS chathub_server_transport)
target_include_directories(chathub_server_transport INTERFACE include)
if(WIN32)
    target_compile_definitions(chathub_server_transport INTERFACE
            _WIN32_WINNT=0x0A00
    )
endif()
target_link_libraries(chathub_server_transport INTERFACE
        chathub::server::application
        asio::asio
        OpenSSL::SSL
        OpenSSL::Crypto
        chathub::contracts
)
# Windows 下 standalone Asio 需要 Winsock；INTERFACE 传递给所有消费者。
if(WIN32)
    target_link_libraries(chathub_server_transport INTERFACE
            ws2_32 mswsock
    )
endif()