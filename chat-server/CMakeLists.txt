# 服务端分层：transport → application → domain ← infrastructure
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
target_link_libraries(chathub_server_transport INTERFACE
        chathub::server::application
)
