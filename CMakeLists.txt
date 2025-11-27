# OpenRTMP - Cross-platform RTMP Server
# Root CMakeLists.txt

cmake_minimum_required(VERSION 3.20)

# Project definition
project(OpenRTMP
    VERSION 0.1.0
    DESCRIPTION "Cross-platform RTMP server library"
    LANGUAGES CXX
)

# =============================================================================
# C++ Standard Configuration
# =============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# Build Options
# =============================================================================

option(OPENRTMP_BUILD_TESTS "Build unit tests" ON)
option(OPENRTMP_BUILD_EXAMPLES "Build example applications" OFF)
option(OPENRTMP_USE_BORINGSSL "Use BoringSSL instead of OpenSSL" OFF)
option(OPENRTMP_ENABLE_RTMPS "Enable RTMPS (TLS) support" ON)
option(OPENRTMP_ENABLE_LOGGING "Enable logging support" ON)

# =============================================================================
# Platform Detection
# =============================================================================

set(OPENRTMP_PLATFORM "UNKNOWN")
set(OPENRTMP_PLATFORM_SOURCES "")
set(OPENRTMP_PLATFORM_LIBS "")
set(OPENRTMP_IS_MOBILE OFF)
set(OPENRTMP_IS_DESKTOP OFF)

# Detect platform
if(APPLE)
    # Check for iOS/iPadOS
    if(IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(OPENRTMP_PLATFORM "iOS")
        set(OPENRTMP_IS_MOBILE ON)
        set(OPENRTMP_PLATFORM_DIR "ios")
        add_compile_definitions(OPENRTMP_PLATFORM_IOS)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "")
        # macOS (Darwin is the kernel name)
        set(OPENRTMP_PLATFORM "macOS")
        set(OPENRTMP_IS_DESKTOP ON)
        set(OPENRTMP_PLATFORM_DIR "macos")
        add_compile_definitions(OPENRTMP_PLATFORM_MACOS)
    endif()

    # Common Apple platform settings
    add_compile_definitions(OPENRTMP_PLATFORM_APPLE)

    # Apple platform libraries
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(SECURITY_FRAMEWORK Security REQUIRED)
    list(APPEND OPENRTMP_PLATFORM_LIBS
        ${FOUNDATION_FRAMEWORK}
        ${SECURITY_FRAMEWORK}
    )

    if(OPENRTMP_IS_MOBILE)
        find_library(NETWORK_FRAMEWORK Network REQUIRED)
        find_library(UIKIT_FRAMEWORK UIKit REQUIRED)
        list(APPEND OPENRTMP_PLATFORM_LIBS
            ${NETWORK_FRAMEWORK}
            ${UIKIT_FRAMEWORK}
        )
    endif()

elseif(WIN32)
    set(OPENRTMP_PLATFORM "Windows")
    set(OPENRTMP_IS_DESKTOP ON)
    set(OPENRTMP_PLATFORM_DIR "windows")
    add_compile_definitions(OPENRTMP_PLATFORM_WINDOWS)

    # Windows platform libraries
    list(APPEND OPENRTMP_PLATFORM_LIBS
        ws2_32
        bcrypt
        crypt32
    )

    # Windows-specific compiler flags
    if(MSVC)
        add_compile_options(/W4 /WX-)
        add_compile_definitions(
            _WIN32_WINNT=0x0A00  # Windows 10
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
    endif()

elseif(ANDROID)
    set(OPENRTMP_PLATFORM "Android")
    set(OPENRTMP_IS_MOBILE ON)
    set(OPENRTMP_PLATFORM_DIR "android")
    add_compile_definitions(OPENRTMP_PLATFORM_ANDROID)

    # Android platform libraries
    list(APPEND OPENRTMP_PLATFORM_LIBS
        log
        android
    )

elseif(UNIX AND NOT APPLE)
    # Linux
    set(OPENRTMP_PLATFORM "Linux")
    set(OPENRTMP_IS_DESKTOP ON)
    set(OPENRTMP_PLATFORM_DIR "linux")
    add_compile_definitions(OPENRTMP_PLATFORM_LINUX)

    # Linux platform libraries
    list(APPEND OPENRTMP_PLATFORM_LIBS
        pthread
    )
endif()

# Report detected platform
message(STATUS "OpenRTMP Platform: ${OPENRTMP_PLATFORM}")
message(STATUS "OpenRTMP Mobile: ${OPENRTMP_IS_MOBILE}")
message(STATUS "OpenRTMP Desktop: ${OPENRTMP_IS_DESKTOP}")

# =============================================================================
# Compiler Configuration
# =============================================================================

# Enable position-independent code for shared library support
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Compiler-specific flags
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wno-unused-parameter
    )

    # Debug flags
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0)
        add_compile_definitions(OPENRTMP_DEBUG)
    endif()

    # Release flags
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3)
        add_compile_definitions(NDEBUG)
    endif()
endif()

# =============================================================================
# Dependencies
# =============================================================================

# Find required packages
find_package(Threads REQUIRED)

# OpenSSL or BoringSSL for TLS support
if(OPENRTMP_ENABLE_RTMPS)
    if(OPENRTMP_USE_BORINGSSL)
        # BoringSSL typically needs to be built from source
        # This is a placeholder - actual BoringSSL integration requires more setup
        message(STATUS "BoringSSL support requested - requires manual configuration")
        add_compile_definitions(OPENRTMP_USE_BORINGSSL)
    else()
        find_package(OpenSSL REQUIRED)
        add_compile_definitions(OPENRTMP_USE_OPENSSL)
    endif()
endif()

# spdlog for logging
if(OPENRTMP_ENABLE_LOGGING)
    find_package(spdlog QUIET)
    if(spdlog_FOUND)
        message(STATUS "Found spdlog")
        add_compile_definitions(OPENRTMP_HAS_SPDLOG)
    else()
        message(STATUS "spdlog not found - using fallback logging")
    endif()
endif()

# Google Test for testing
if(OPENRTMP_BUILD_TESTS)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        message(STATUS "Found Google Test")
        enable_testing()
    else()
        message(STATUS "Google Test not found - tests will be skipped")
        set(OPENRTMP_BUILD_TESTS OFF)
    endif()
endif()

# =============================================================================
# Include Directories
# =============================================================================

# Add include directory for all targets
include_directories(${CMAKE_SOURCE_DIR}/include)

# =============================================================================
# Source Files
# =============================================================================

# Core library sources (platform-independent)
set(OPENRTMP_CORE_SOURCES
    src/core/buffer.cpp
    src/core/types.cpp
)

# Protocol layer sources (platform-independent)
set(OPENRTMP_PROTOCOL_SOURCES
    src/protocol/handshake_handler.cpp
    src/protocol/chunk_parser.cpp
    src/protocol/amf_codec.cpp
)

# Streaming layer sources (platform-independent)
set(OPENRTMP_STREAMING_SOURCES
    src/streaming/stream_registry.cpp
    src/streaming/media_handler.cpp
)

# Platform-specific sources will be added by subdirectories
file(GLOB OPENRTMP_PAL_SOURCES
    "src/pal/${OPENRTMP_PLATFORM_DIR}/*.cpp"
)

# For Apple platforms (macOS/iOS), use Darwin PAL implementation
if(APPLE)
    file(GLOB OPENRTMP_DARWIN_PAL_SOURCES
        "src/pal/darwin/*.cpp"
    )
    list(APPEND OPENRTMP_PAL_SOURCES ${OPENRTMP_DARWIN_PAL_SOURCES})
    message(STATUS "Darwin PAL sources: ${OPENRTMP_DARWIN_PAL_SOURCES}")
endif()

# For Windows, use Windows PAL implementation with IOCP
if(WIN32)
    file(GLOB OPENRTMP_WINDOWS_PAL_SOURCES
        "src/pal/windows/*.cpp"
    )
    list(APPEND OPENRTMP_PAL_SOURCES ${OPENRTMP_WINDOWS_PAL_SOURCES})
    message(STATUS "Windows PAL sources: ${OPENRTMP_WINDOWS_PAL_SOURCES}")

    # Add mswsock library for AcceptEx
    list(APPEND OPENRTMP_PLATFORM_LIBS mswsock)
endif()

# For Linux/Android, use Linux PAL implementation with epoll
if(UNIX AND NOT APPLE)
    file(GLOB OPENRTMP_LINUX_PAL_SOURCES
        "src/pal/linux/*.cpp"
    )
    list(APPEND OPENRTMP_PAL_SOURCES ${OPENRTMP_LINUX_PAL_SOURCES})
    message(STATUS "Linux PAL sources: ${OPENRTMP_LINUX_PAL_SOURCES}")
endif()

# =============================================================================
# Library Targets
# =============================================================================

# Core library (platform-independent)
add_library(openrtmp_core STATIC
    ${OPENRTMP_CORE_SOURCES}
)

target_include_directories(openrtmp_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(openrtmp_core
    PUBLIC
        Threads::Threads
)

# Main OpenRTMP library
add_library(openrtmp STATIC
    ${OPENRTMP_CORE_SOURCES}
    ${OPENRTMP_PROTOCOL_SOURCES}
    ${OPENRTMP_STREAMING_SOURCES}
    ${OPENRTMP_PAL_SOURCES}
)

target_include_directories(openrtmp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(openrtmp
    PUBLIC
        openrtmp_core
        Threads::Threads
        ${OPENRTMP_PLATFORM_LIBS}
)

if(OPENRTMP_ENABLE_RTMPS AND OpenSSL_FOUND)
    target_link_libraries(openrtmp
        PUBLIC
            OpenSSL::SSL
            OpenSSL::Crypto
    )
endif()

if(spdlog_FOUND)
    target_link_libraries(openrtmp
        PUBLIC
            spdlog::spdlog
    )
endif()

# =============================================================================
# Tests
# =============================================================================

if(OPENRTMP_BUILD_TESTS AND GTest_FOUND)
    add_subdirectory(tests)
endif()

# =============================================================================
# Installation
# =============================================================================

include(GNUInstallDirs)

# Install libraries
install(TARGETS openrtmp openrtmp_core
    EXPORT OpenRTMPTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install headers
install(DIRECTORY include/openrtmp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Export targets
install(EXPORT OpenRTMPTargets
    FILE OpenRTMPTargets.cmake
    NAMESPACE OpenRTMP::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OpenRTMP
)

# =============================================================================
# Summary
# =============================================================================

message(STATUS "")
message(STATUS "OpenRTMP Configuration Summary")
message(STATUS "==============================")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Platform: ${OPENRTMP_PLATFORM}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Build Tests: ${OPENRTMP_BUILD_TESTS}")
message(STATUS "RTMPS Support: ${OPENRTMP_ENABLE_RTMPS}")
message(STATUS "Logging Support: ${OPENRTMP_ENABLE_LOGGING}")
message(STATUS "")
