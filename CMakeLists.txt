cmake_minimum_required(VERSION 3.21)
project(chathub LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 根目录只保存跨模块规则：语言标准、测试总开关和模块边界。
# 每个模块的依赖、可执行程序及测试目标由自己的 CMakeLists.txt 管理。
enable_testing()

add_subdirectory(chat-server)
add_subdirectory(client-qt)
