# OpenRTMP - Windows IOCP Research Spike
# CMake build configuration for proof-of-concept and benchmark

cmake_minimum_required(VERSION 3.20)
project(windows_iocp_spike VERSION 1.0.0 LANGUAGES CXX)

# Require C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Only build on Windows
if(NOT WIN32)
    message(STATUS "IOCP research spike is Windows-only. Skipping.")
    return()
endif()

# Compiler flags for Windows
if(MSVC)
    add_compile_options(/W4 /WX- /O2 /Zi)
    add_compile_definitions(_WIN32_WINNT=0x0601)  # Windows 7+
else()
    add_compile_options(-Wall -Wextra -O2)
endif()

# Windows libraries
set(WINDOWS_LIBS
    ws2_32      # Winsock 2
    mswsock     # Microsoft Winsock extensions (AcceptEx)
)

# ============================================================================
# IOCP Echo Server (Header-only library)
# ============================================================================
add_library(iocp_echo_server INTERFACE)
target_include_directories(iocp_echo_server INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# ============================================================================
# WSAPoll Echo Server (Header-only library)
# ============================================================================
add_library(wsapoll_echo_server INTERFACE)
target_include_directories(wsapoll_echo_server INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# ============================================================================
# Benchmark Executable
# ============================================================================
add_executable(benchmark_test benchmark_test.cpp)
target_link_libraries(benchmark_test PRIVATE
    iocp_echo_server
    wsapoll_echo_server
    ${WINDOWS_LIBS}
)

# ============================================================================
# Unit Tests (if Google Test is available)
# ============================================================================
find_package(GTest QUIET)

if(GTest_FOUND)
    enable_testing()

    add_executable(iocp_test
        tests/iocp_server_test.cpp
    )
    target_link_libraries(iocp_test PRIVATE
        iocp_echo_server
        GTest::gtest_main
        ${WINDOWS_LIBS}
    )
    add_test(NAME IOCPServerTests COMMAND iocp_test)

    add_executable(wsapoll_test
        tests/wsapoll_server_test.cpp
    )
    target_link_libraries(wsapoll_test PRIVATE
        wsapoll_echo_server
        GTest::gtest_main
        ${WINDOWS_LIBS}
    )
    add_test(NAME WSAPollServerTests COMMAND wsapoll_test)

else()
    message(STATUS "Google Test not found. Unit tests will not be built.")
    message(STATUS "To build tests, install Google Test and rerun cmake.")
endif()

# ============================================================================
# Installation (optional)
# ============================================================================
install(TARGETS benchmark_test
    RUNTIME DESTINATION bin
)

install(FILES
    iocp_echo_server.hpp
    wsapoll_echo_server.hpp
    DESTINATION include/research
)

# ============================================================================
# Documentation
# ============================================================================
message(STATUS "")
message(STATUS "=== Windows IOCP Research Spike ===")
message(STATUS "")
message(STATUS "Build the benchmark:")
message(STATUS "  cmake --build . --target benchmark_test")
message(STATUS "")
message(STATUS "Run the benchmark:")
message(STATUS "  ./benchmark_test")
message(STATUS "")
message(STATUS "Expected output: Comparison of IOCP vs WSAPoll at 100/500/1000 connections")
message(STATUS "")
