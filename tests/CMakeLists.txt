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

# Create test executable
add_executable(openrtmp_tests ${TEST_SOURCES})

# Link against OpenRTMP library and GTest
target_link_libraries(openrtmp_tests
    PRIVATE
        openrtmp_core
        GTest::GTest
        GTest::Main
)

# Include directories
target_include_directories(openrtmp_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

# Enable testing
include(GoogleTest)
gtest_discover_tests(openrtmp_tests)
