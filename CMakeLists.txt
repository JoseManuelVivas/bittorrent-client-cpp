cmake_minimum_required(VERSION 3.20)

project(bittorrent_client
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # Strict ISO C++, no GNU extensions

# Default to Debug for single-config generators when no build type is given
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(ENABLE_SANITIZERS  "Enable ASan and UBSan in Debug builds" ON)

# ----------------------------------------------------------------------------
# Shared build settings: every project target links this, so warnings and
# sanitizers are defined once and applied everywhere.
# ----------------------------------------------------------------------------
add_library(btclient_options INTERFACE)

target_compile_options(btclient_options INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion  # Not covered by -Wconversion in GCC; catches int/size_t mixups
    -Wshadow
)

if(WARNINGS_AS_ERRORS)
    target_compile_options(btclient_options INTERFACE -Werror)
endif()

# The client parses untrusted binary data from the network, so memory and
# UB errors must fail loudly during development.
if(ENABLE_SANITIZERS)
    set(SANITIZER_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
    # Needed at both compile and link time (links the sanitizer runtime)
    target_compile_options(btclient_options INTERFACE "$<$<CONFIG:Debug>:${SANITIZER_FLAGS}>")
    target_link_options(btclient_options INTERFACE "$<$<CONFIG:Debug>:${SANITIZER_FLAGS}>")
endif()

# ----------------------------------------------------------------------------
# Core library: protocol logic, reused by the client, tools and tests
# ----------------------------------------------------------------------------
add_library(btcore STATIC
    src/bencode.cpp
    src/bencode_printer.cpp
)

target_include_directories(btcore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(btcore PRIVATE btclient_options)

# ----------------------------------------------------------------------------
# Client executable
# ----------------------------------------------------------------------------
add_executable(bittorrent_client
    src/main.cpp
)

target_link_libraries(bittorrent_client PRIVATE
    btcore
    btclient_options
)

# ----------------------------------------------------------------------------
# Tests (GoogleTest)
# ----------------------------------------------------------------------------
# GoogleTest is downloaded at configure time instead of relying on a system
# install. The release is pinned and verified by its SHA-256 hash, so the build
# is reproducible and a tampered or corrupted archive is rejected.
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(btclient_tests
    tests/bencode_test.cpp
)
target_link_libraries(btclient_tests PRIVATE
    btcore
    btclient_options
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(btclient_tests)
