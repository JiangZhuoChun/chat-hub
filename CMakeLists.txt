cmake_minimum_required(VERSION 3.23)

project(ChatHub
    VERSION 1.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_subdirectory(shared)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif ()
