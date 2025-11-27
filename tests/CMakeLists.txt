# OpenRTMP - Cross-platform RTMP Server
# Tests CMakeLists.txt

# Find Google Test
find_package(GTest REQUIRED)

# Test sources
set(TEST_SOURCES
    core/result_test.cpp
    core/buffer_test.cpp
    pal/pal_interface_test.cpp
)

# Platform-specific test sources
if(APPLE)
    # Darwin PAL tests for macOS/iOS
    list(APPEND TEST_SOURCES
        pal/darwin/darwin_log_pal_test.cpp
        pal/darwin/darwin_timer_pal_test.cpp
        pal/darwin/darwin_thread_pal_test.cpp
        pal/darwin/darwin_network_pal_test.cpp
    )
    message(STATUS "Adding Darwin PAL tests")
endif()

if(WIN32)
    # Windows PAL tests using IOCP
    list(APPEND TEST_SOURCES
        pal/windows/windows_log_pal_test.cpp
        pal/windows/windows_timer_pal_test.cpp
        pal/windows/windows_thread_pal_test.cpp
        pal/windows/windows_network_pal_test.cpp
    )
    message(STATUS "Adding Windows PAL tests")
endif()

# Create test executable
add_executable(openrtmp_tests ${TEST_SOURCES})

# Link against OpenRTMP library and GTest
target_link_libraries(openrtmp_tests
    PRIVATE
        openrtmp
        GTest::GTest
        GTest::Main
)

# Include directories
target_include_directories(openrtmp_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

# Platform-specific libraries for tests
if(APPLE)
    target_link_libraries(openrtmp_tests
        PRIVATE
            "-framework Foundation"
            "-framework CoreFoundation"
    )
endif()

if(WIN32)
    target_link_libraries(openrtmp_tests
        PRIVATE
            ws2_32
            mswsock
    )
endif()

# Enable testing
include(GoogleTest)
gtest_discover_tests(openrtmp_tests)
