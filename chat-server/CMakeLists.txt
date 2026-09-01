cmake_minimum_required(VERSION 3.21)

# 只在本目录初始化服务端依赖和通用目标配置；具体目标在子目录中声明。
set(CHATHUB_CHAT_SERVER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
include(cmake/Dependencies.cmake)
include(cmake/TargetHelpers.cmake)

# 生产程序、独立示例、CTest 测试分别维护，避免测试清单淹没服务端入口。
add_subdirectory(app)
add_subdirectory(examples)
add_subdirectory(tests)
