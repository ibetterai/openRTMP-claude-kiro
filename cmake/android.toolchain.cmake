# OpenRTMP Android Toolchain
# CMake toolchain file for Android cross-compilation
# Note: This is a wrapper that should be used with the Android NDK toolchain

# Check for NDK
if(NOT DEFINED ANDROID_NDK)
    if(DEFINED ENV{ANDROID_NDK})
        set(ANDROID_NDK $ENV{ANDROID_NDK})
    elseif(DEFINED ENV{ANDROID_NDK_HOME})
        set(ANDROID_NDK $ENV{ANDROID_NDK_HOME})
    else()
        message(FATAL_ERROR "ANDROID_NDK not set. Please set ANDROID_NDK environment variable or CMake variable.")
    endif()
endif()

# Include the NDK toolchain
include(${ANDROID_NDK}/build/cmake/android.toolchain.cmake)

# OpenRTMP specific settings
set(CMAKE_SYSTEM_NAME Android)

# Minimum API level (Android 8.0)
if(NOT DEFINED ANDROID_NATIVE_API_LEVEL)
    set(ANDROID_NATIVE_API_LEVEL 26)
endif()

# Default architecture
if(NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI "arm64-v8a")
endif()

# STL configuration
if(NOT DEFINED ANDROID_STL)
    set(ANDROID_STL "c++_shared")
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions -frtti")

# Disable tests for cross-compilation
set(OPENRTMP_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)

# BoringSSL is typically preferred on Android
set(OPENRTMP_USE_BORINGSSL ON CACHE BOOL "Use BoringSSL" FORCE)
