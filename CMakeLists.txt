# SPDX-License-Identifier: MIT
# © WinUDPShardedEcho Contributors. All rights reserved.

cmake_minimum_required(VERSION 3.20)

project(WinUDPShardedEcho LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Install developer convenience scripts (pre-commit hook) only for the top-level project
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  include(cmake/format.cmake OPTIONAL)
endif()

# Prefer to use FetchContent to bring in Microsoft WIL (Windows Implementation Libraries)
include(FetchContent)

FetchContent_Declare(
    wil
    GIT_REPOSITORY https://github.com/microsoft/wil.git
    GIT_TAG        b6ec76a2d9a609897f25a7fa0a0bdf4238e94e35  # resolved SHA for v1.0.250325.1
)

set(WIL_BUILD_TESTS OFF CACHE BOOL "Disable WIL tests")
set(WIL_BUILD_DOCS OFF CACHE BOOL "Disable WIL docs")

FetchContent_MakeAvailable(wil)

# Server executable
add_executable(echo_server
    src/server/main.cpp
    src/common/socket_utils.cpp
)

target_include_directories(echo_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${wil_SOURCE_DIR}/include
)

# Client executable
add_executable(echo_client
    src/client/main.cpp
    src/common/socket_utils.cpp
)

target_include_directories(echo_client PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${wil_SOURCE_DIR}/include
)

# Windows-specific libraries
if(WIN32)
    target_link_libraries(echo_server PRIVATE ws2_32)
    target_link_libraries(echo_client PRIVATE ws2_32)
endif()
