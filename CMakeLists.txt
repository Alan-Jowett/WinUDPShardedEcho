cmake_minimum_required(VERSION 3.20)

project(scalable_echo_server_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Server executable
add_executable(echo_server
    src/server/main.cpp
    src/common/socket_utils.cpp
)

target_include_directories(echo_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Client executable
add_executable(echo_client
    src/client/main.cpp
    src/common/socket_utils.cpp
)

target_include_directories(echo_client PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Windows-specific libraries
if(WIN32)
    target_link_libraries(echo_server PRIVATE ws2_32)
    target_link_libraries(echo_client PRIVATE ws2_32)
endif()
