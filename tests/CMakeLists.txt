# OpenRTMP - Cross-platform RTMP Server
# Tests CMakeLists.txt

# Find Google Test
find_package(GTest REQUIRED)

# Test sources
set(TEST_SOURCES
    core/result_test.cpp
    core/buffer_test.cpp
    core/auth_service_test.cpp
    core/external_auth_callback_test.cpp
    core/access_control_test.cpp
    core/tls_service_test.cpp
    core/config_manager_test.cpp
    core/structured_logger_test.cpp
    core/log_rotation_test.cpp
    core/metrics_collector_test.cpp
    core/connection_pool_test.cpp
    core/publisher_limit_test.cpp
    core/shutdown_coordinator_test.cpp
    core/error_isolation_test.cpp
    pal/pal_interface_test.cpp
    protocol/handshake_handler_test.cpp
    protocol/chunk_parser_test.cpp
    protocol/amf_codec_test.cpp
    protocol/message_assembler_test.cpp
    protocol/command_handler_test.cpp
    streaming/stream_registry_test.cpp
    streaming/media_handler_test.cpp
    streaming/gop_buffer_test.cpp
    streaming/session_test.cpp
    streaming/publisher_lifecycle_test.cpp
    streaming/subscriber_buffer_test.cpp
    streaming/subscriber_manager_test.cpp
    streaming/media_distribution_test.cpp
    streaming/latency_optimizer_test.cpp
    streaming/throughput_optimizer_test.cpp
    api/rtmp_server_test.cpp
    api/server_lifecycle_test.cpp
    # Integration tests (Task 21.2)
    integration/publish_flow_test.cpp
    integration/subscribe_flow_test.cpp
    integration/multi_subscriber_test.cpp
    integration/auth_callback_test.cpp
    integration/tls_connection_test.cpp
    # E2E tests (Task 21.3)
    e2e/obs_compatibility_test.cpp
    e2e/ffmpeg_roundtrip_test.cpp
    e2e/mobile_lifecycle_test.cpp
    e2e/network_failover_test.cpp
    # Performance tests (Task 21.3)
    performance/scalability_test.cpp
    performance/throughput_test.cpp
    performance/latency_test.cpp
)

# Platform-specific test sources
if(APPLE)
    # Darwin PAL tests for macOS/iOS
    list(APPEND TEST_SOURCES
        pal/darwin/darwin_log_pal_test.cpp
        pal/darwin/darwin_timer_pal_test.cpp
        pal/darwin/darwin_thread_pal_test.cpp
        pal/darwin/darwin_network_pal_test.cpp
        pal/darwin/darwin_service_wrapper_test.cpp
    )
    message(STATUS "Adding Darwin PAL tests")

    # iOS-specific PAL tests (also compiled on macOS for mock testing)
    list(APPEND TEST_SOURCES
        pal/ios/ios_background_task_test.cpp
    )
    message(STATUS "Adding iOS PAL tests")

    # Android-specific PAL tests (also compiled on macOS for mock testing)
    list(APPEND TEST_SOURCES
        pal/android/android_foreground_service_test.cpp
    )
    message(STATUS "Adding Android PAL tests (mock on macOS)")

    # Mobile battery optimization tests (also compiled on macOS for mock testing)
    list(APPEND TEST_SOURCES
        pal/mobile/battery_optimization_test.cpp
    )
    message(STATUS "Adding Mobile battery optimization tests")

    # Mobile network monitor tests (also compiled on macOS for mock testing)
    list(APPEND TEST_SOURCES
        pal/mobile/network_monitor_test.cpp
    )
    message(STATUS "Adding Mobile network monitor tests")
endif()

if(WIN32)
    # Windows PAL tests using IOCP
    list(APPEND TEST_SOURCES
        pal/windows/windows_log_pal_test.cpp
        pal/windows/windows_timer_pal_test.cpp
        pal/windows/windows_thread_pal_test.cpp
        pal/windows/windows_network_pal_test.cpp
        pal/windows/windows_service_wrapper_test.cpp
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
        pal/linux/linux_service_wrapper_test.cpp
    )
    message(STATUS "Adding Linux PAL tests")

    # Android-specific PAL tests (also compiled on Linux for mock testing)
    list(APPEND TEST_SOURCES
        pal/android/android_foreground_service_test.cpp
    )
    message(STATUS "Adding Android PAL tests")

    # Mobile battery optimization tests (also compiled on Linux for mock testing)
    list(APPEND TEST_SOURCES
        pal/mobile/battery_optimization_test.cpp
    )
    message(STATUS "Adding Mobile battery optimization tests")

    # Mobile network monitor tests (also compiled on Linux for mock testing)
    list(APPEND TEST_SOURCES
        pal/mobile/network_monitor_test.cpp
    )
    message(STATUS "Adding Mobile network monitor tests")
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
    # Add compile definition to enable iOS tests on macOS via mock
    target_compile_definitions(openrtmp_tests PRIVATE OPENRTMP_IOS_TEST=1)
    # Add compile definition to enable Android tests on macOS via mock
    target_compile_definitions(openrtmp_tests PRIVATE OPENRTMP_ANDROID_TEST=1)
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
    # Add compile definition to enable Android tests on Linux via mock
    target_compile_definitions(openrtmp_tests PRIVATE OPENRTMP_ANDROID_TEST=1)
endif()

# Enable testing
include(GoogleTest)
gtest_discover_tests(openrtmp_tests)
