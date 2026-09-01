# 客户端分层：presentation → application → domain ← infrastructure
add_library(chathub_client_domain INTERFACE)
add_library(chathub::client::domain ALIAS chathub_client_domain)
target_compile_features(chathub_client_domain INTERFACE cxx_std_20)

add_library(chathub_client_application INTERFACE)
add_library(chathub::client::application ALIAS chathub_client_application)
target_link_libraries(chathub_client_application INTERFACE
        chathub::client::domain
)

add_library(chathub_client_infrastructure INTERFACE)
add_library(chathub::client::infrastructure ALIAS chathub_client_infrastructure)
target_link_libraries(chathub_client_infrastructure INTERFACE
        chathub::client::domain
)

add_library(chathub_client_presentation INTERFACE)
add_library(chathub::client::presentation ALIAS chathub_client_presentation)
target_link_libraries(chathub_client_presentation INTERFACE
        chathub::client::application
)