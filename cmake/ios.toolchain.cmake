# OpenRTMP iOS Toolchain
# CMake toolchain file for iOS cross-compilation

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_VERSION 15.0)

# Deployment target
set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "iOS deployment target")

# Architecture (arm64 for device, x86_64 for simulator)
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "iOS architecture")
endif()

# SDK selection based on architecture
if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
    set(CMAKE_OSX_SYSROOT "iphonesimulator" CACHE STRING "iOS SDK")
else()
    set(CMAKE_OSX_SYSROOT "iphoneos" CACHE STRING "iOS SDK")
endif()

# Compiler flags
set(CMAKE_C_FLAGS_INIT "-fembed-bitcode")
set(CMAKE_CXX_FLAGS_INIT "-fembed-bitcode")

# Find root compiler
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Skip TRY_COMPILE tests for cross-compilation
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Disable tests for cross-compilation
set(OPENRTMP_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
