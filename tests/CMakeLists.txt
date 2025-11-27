# OpenRTMP - Cross-platform RTMP Server
# Tests CMakeLists.txt

# Find Google Test
find_package(GTest REQUIRED)

# Test sources
set(TEST_SOURCES
    core/result_test.cpp
    core/buffer_test.cpp
    pal/pal_interface_test.cpp
    protocol/handshake_handler_test.cpp
    protocol/chunk_parser_test.cpp
    protocol/amf_codec_test.cpp
    streaming/stream_registry_test.cpp
    streaming/media_handler_test.cpp
    streaming/gop_buffer_test.cpp
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

if(UNIX AND NOT APPLE)
    # Linux/Android PAL tests using epoll
    list(APPEND TEST_SOURCES
        pal/linux/linux_log_pal_test.cpp
        pal/linux/linux_timer_pal_test.cpp
        pal/linux/linux_thread_pal_test.cpp
        pal/linux/linux_network_pal_test.cpp
    )
    message(STATUS "Adding Linux PAL tests")
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

if(UNIX AND NOT APPLE)
    target_link_libraries(openrtmp_tests
        PRIVATE
            pthread
    )
endif()

# Enable testing
include(GoogleTest)
gtest_discover_tests(openrtmp_tests)
