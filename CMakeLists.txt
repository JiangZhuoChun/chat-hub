# 项目基础配置：固定最低 CMake 版本、项目元数据和标准 C++ 模式。
cmake_minimum_required(VERSION 3.23)

project(ChatHub
    VERSION 1.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_EXTENSIONS OFF)

# Qt 全局配置：统一版本下限，并为后续 Qt target 启用标准工程设置。
find_package(Qt6 6.11.1 REQUIRED COMPONENTS Core Network)
qt_standard_project_setup(REQUIRES 6.11.1)

# 测试基础设施：提供 BUILD_TESTING 选项和 CTest 注册能力。
include(CTest)

# 子项目装配：按依赖层次注册公共合同、服务端、客户端和开发工具。
add_subdirectory(shared)
add_subdirectory(chat-server)
add_subdirectory(client-qt)
add_subdirectory(tools/pki)

# 测试目标只在 BUILD_TESTING 开启时进入构建图。
if (BUILD_TESTING)
    add_subdirectory(tests)
endif ()
