# 独立 JWT 演示不是 CTest；它保留为手动学习/排错入口。
add_executable(jwt_demo jwt_demo.cpp)
configure_chat_server_target(jwt_demo)
target_link_libraries(jwt_demo PRIVATE OpenSSL::Crypto)
link_chat_server_jwt(jwt_demo)
