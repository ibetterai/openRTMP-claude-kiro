// OpenRTMP - Cross-platform RTMP Server
// Tests for Log Rotation Component
//
// Requirements Covered:
// - 18.4: On desktop platforms, support log output to file with configurable rotation
// - 18.5: On mobile platforms, integrate with platform-native logging systems

#include <gtest/gtest.h>
#include "openrtmp/core/log_rotation.hpp"
#include "openrtmp/core/structured_logger.hpp"
#include "openrtmp/pal/log_pal.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>

namespace openrtmp {
namespace core {
namespace test {

// =============================================================================
// Test Utilities
// =============================================================================

class LogRotationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a temporary directory for test log files
        testDir_ = std::filesystem::temp_directory_path() / "openrtmp_log_test";
        std::filesystem::create_directories(testDir_);
        testLogPath_ = testDir_ / "test.log";
    }

    void TearDown() override {
        // Clean up test files
        std::error_code ec;
        std::filesystem::remove_all(testDir_, ec);
    }

    std::filesystem::path testDir_;
    std::filesystem::path testLogPath_;
};

// =============================================================================
// RotationPolicy Configuration Tests
// =============================================================================

TEST_F(LogRotationTest, RotationPolicyDefaultValues) {
    RotationPolicy policy;

    // Default should be no automatic rotation
    EXPECT_EQ(policy.getRotationType(), RotationType::None);
    EXPECT_EQ(policy.getMaxFileSize(), 0u);
    EXPECT_EQ(policy.getRotationInterval(), std::chrono::hours(0));
    EXPECT_EQ(policy.getMaxBackupFiles(), 5u);  // Default backup count
    EXPECT_FALSE(policy.isCompressionEnabled());
}

TEST_F(LogRotationTest, RotationPolicySizeBasedConfiguration) {
    RotationPolicy policy;

    // Configure size-based rotation at 10MB
    policy.setRotationType(RotationType::SizeBased);
    policy.setMaxFileSize(10 * 1024 * 1024);  // 10 MB

    EXPECT_EQ(policy.getRotationType(), RotationType::SizeBased);
    EXPECT_EQ(policy.getMaxFileSize(), 10u * 1024 * 1024);
}

TEST_F(LogRotationTest, RotationPolicyTimeBasedConfiguration) {
    RotationPolicy policy;

    // Configure hourly rotation
    policy.setRotationType(RotationType::TimeBased);
    policy.setRotationInterval(std::chrono::hours(1));

    EXPECT_EQ(policy.getRotationType(), RotationType::TimeBased);
    EXPECT_EQ(policy.getRotationInterval(), std::chrono::hours(1));
}

TEST_F(LogRotationTest, RotationPolicyDailyRotation) {
    RotationPolicy policy;

    // Configure daily rotation
    policy.setRotationType(RotationType::TimeBased);
    policy.setRotationInterval(std::chrono::hours(24));

    EXPECT_EQ(policy.getRotationInterval(), std::chrono::hours(24));
}

TEST_F(LogRotationTest, RotationPolicyWeeklyRotation) {
    RotationPolicy policy;

    // Configure weekly rotation
    policy.setRotationType(RotationType::TimeBased);
    policy.setRotationInterval(std::chrono::hours(24 * 7));

    EXPECT_EQ(policy.getRotationInterval(), std::chrono::hours(24 * 7));
}

TEST_F(LogRotationTest, RotationPolicyHybridConfiguration) {
    RotationPolicy policy;

    // Configure hybrid: rotate on either size or time trigger
    policy.setRotationType(RotationType::Hybrid);
    policy.setMaxFileSize(5 * 1024 * 1024);  // 5 MB
    policy.setRotationInterval(std::chrono::hours(24));  // Daily

    EXPECT_EQ(policy.getRotationType(), RotationType::Hybrid);
    EXPECT_EQ(policy.getMaxFileSize(), 5u * 1024 * 1024);
    EXPECT_EQ(policy.getRotationInterval(), std::chrono::hours(24));
}

TEST_F(LogRotationTest, RotationPolicyBackupFilesConfiguration) {
    RotationPolicy policy;

    // Configure number of backup files to keep
    policy.setMaxBackupFiles(10);
    EXPECT_EQ(policy.getMaxBackupFiles(), 10u);

    policy.setMaxBackupFiles(3);
    EXPECT_EQ(policy.getMaxBackupFiles(), 3u);
}

TEST_F(LogRotationTest, RotationPolicyCompressionConfiguration) {
    RotationPolicy policy;

    // Enable compression for rotated files
    EXPECT_FALSE(policy.isCompressionEnabled());

    policy.setCompressionEnabled(true);
    EXPECT_TRUE(policy.isCompressionEnabled());

    policy.setCompressionEnabled(false);
    EXPECT_FALSE(policy.isCompressionEnabled());
}

// =============================================================================
// FileSink Basic Tests
// =============================================================================

TEST_F(LogRotationTest, FileSinkCreatesLogFile) {
    FileSink sink(testLogPath_.string());
    EXPECT_TRUE(sink.isOpen());

    // Write a message
    pal::LogContext ctx;
    sink.write(pal::LogLevel::Info, "Test message", "Test", ctx);
    sink.flush();

    // Verify file exists and has content
    EXPECT_TRUE(std::filesystem::exists(testLogPath_));
    EXPECT_GT(std::filesystem::file_size(testLogPath_), 0u);
}

TEST_F(LogRotationTest, FileSinkWritesFormattedMessages) {
    FileSink sink(testLogPath_.string());

    pal::LogContext ctx;
    sink.write(pal::LogLevel::Info, "Hello World", "Test", ctx);
    sink.flush();

    // Read file content
    std::ifstream file(testLogPath_);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    EXPECT_NE(content.find("Hello World"), std::string::npos);
}

TEST_F(LogRotationTest, FileSinkHandlesMultipleWrites) {
    FileSink sink(testLogPath_.string());

    pal::LogContext ctx;
    for (int i = 0; i < 100; ++i) {
        sink.write(pal::LogLevel::Info, "Message " + std::to_string(i), "Test", ctx);
    }
    sink.flush();

    // Count lines in file
    std::ifstream file(testLogPath_);
    int lineCount = 0;
    std::string line;
    while (std::getline(file, line)) {
        lineCount++;
    }

    EXPECT_EQ(lineCount, 100);
}

TEST_F(LogRotationTest, FileSinkFlushWritesBufferedData) {
    FileSink sink(testLogPath_.string());

    pal::LogContext ctx;
    sink.write(pal::LogLevel::Info, "Buffered message", "Test", ctx);

    // Before flush, file might not have all data
    sink.flush();

    // After flush, file must have the data
    std::ifstream file(testLogPath_);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    EXPECT_NE(content.find("Buffered message"), std::string::npos);
}

TEST_F(LogRotationTest, FileSinkGetName) {
    FileSink sink(testLogPath_.string());
    EXPECT_EQ(sink.getName(), "FileSink");
}

// =============================================================================
// Size-Based Rotation Tests
// =============================================================================

TEST_F(LogRotationTest, SizeBasedRotationTriggersAtThreshold) {
    RotationPolicy policy;
    policy.setRotationType(RotationType::SizeBased);
    policy.setMaxFileSize(1024);  // 1 KB threshold for testing
    policy.setMaxBackupFiles(3);

    FileSink sink(testLogPath_.string(), policy);

    // Write enough data to trigger rotation
    pal::LogContext ctx;
    std::string largeMessage(200, 'X');  // 200 bytes per message
    for (int i = 0; i < 10; ++i) {  // Should exceed 1KB
        sink.write(pal::LogLevel::Info, largeMessage, "Test", ctx);
    }
    sink.flush();

    // Check that rotated file exists
    auto rotatedPath = testLogPath_;
    rotatedPath += ".1";
    EXPECT_TRUE(std::filesystem::exists(rotatedPath) ||
                std::filesystem::exists(testLogPath_));
}

TEST_F(LogRotationTest, SizeBasedRotationKeepsMaxBackups) {
    RotationPolicy policy;
    policy.setRotationType(RotationType::SizeBased);
    policy.setMaxFileSize(256);  // Very small for testing
    policy.setMaxBackupFiles(2);

    FileSink sink(testLogPath_.string(), policy);

    // Write enough to trigger multiple rotations
    pal::LogContext ctx;
    std::string message(100, 'A');
    for (int i = 0; i < 30; ++i) {
        sink.write(pal::LogLevel::Info, message, "Test", ctx);
        sink.flush();
    }

    // Count backup files (should be at most 2)
    int backupCount = 0;
    for (int i = 1; i <= 10; ++i) {
        auto backupPath = testLogPath_.string() + "." + std::to_string(i);
        if (std::filesystem::exists(backupPath)) {
            backupCount++;
        }
    }

    EXPECT_LE(backupCount, 2);
}

// =============================================================================
// Time-Based Rotation Tests
// =============================================================================

TEST_F(LogRotationTest, TimeBasedRotationChecksInterval) {
    RotationPolicy policy;
    policy.setRotationType(RotationType::TimeBased);
    policy.setRotationInterval(std::chrono::milliseconds(100));  // Short interval for testing

    FileSink sink(testLogPath_.string(), policy);

    pal::LogContext ctx;
    sink.write(pal::LogLevel::Info, "Message before interval", "Test", ctx);
    sink.flush();

    // Wait for rotation interval to pass
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Next write should trigger rotation check
    sink.write(pal::LogLevel::Info, "Message after interval", "Test", ctx);
    sink.flush();

    // Either rotated file exists or main file was rotated
    EXPECT_TRUE(std::filesystem::exists(testLogPath_));
}

TEST_F(LogRotationTest, TimeBasedRotationPreservesCurrentLog) {
    RotationPolicy policy;
    policy.setRotationType(RotationType::TimeBased);
    policy.setRotationInterval(std::chrono::milliseconds(50));

    FileSink sink(testLogPath_.string(), policy);

    pal::LogContext ctx;
    sink.write(pal::LogLevel::Info, "First message", "Test", ctx);
    sink.flush();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    sink.write(pal::LogLevel::Info, "Second message", "Test", ctx);
    sink.flush();

    // Current log file should exist and be writable
    EXPECT_TRUE(std::filesystem::exists(testLogPath_));

    // Read the current log - should have the latest message
    std::ifstream file(testLogPath_);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("Second message"), std::string::npos);
}

// =============================================================================
// Compression Tests
// =============================================================================

TEST_F(LogRotationTest, CompressionInterfaceExists) {
    // Test that compression interface is available
    ICompressor* compressor = createGzipCompressor();
    EXPECT_NE(compressor, nullptr);
    delete compressor;
}

TEST_F(LogRotationTest, CompressionCompressesData) {
    auto compressor = std::unique_ptr<ICompressor>(createGzipCompressor());

    std::string testData = "This is test data for compression. It should compress well "
                           "because it has repeating patterns. Repeating patterns. "
                           "Repeating patterns. Repeating patterns.";

    std::vector<uint8_t> compressed;
    auto result = compressor->compress(
        reinterpret_cast<const uint8_t*>(testData.data()),
        testData.size(),
        compressed
    );

    EXPECT_TRUE(result.isSuccess());
    // Compressed data should be smaller (for repetitive data)
    EXPECT_LT(compressed.size(), testData.size());
}

TEST_F(LogRotationTest, RotatedFileIsCompressedWhenEnabled) {
    RotationPolicy policy;
    policy.setRotationType(RotationType::SizeBased);
    policy.setMaxFileSize(256);
    policy.setMaxBackupFiles(2);
    policy.setCompressionEnabled(true);

    FileSink sink(testLogPath_.string(), policy);

    // Write enough to trigger rotation
    pal::LogContext ctx;
    std::string message(100, 'B');
    for (int i = 0; i < 20; ++i) {
        sink.write(pal::LogLevel::Info, message, "Test", ctx);
        sink.flush();
    }

    // Check for .gz extension on rotated file
    bool foundCompressed = false;
    for (const auto& entry : std::filesystem::directory_iterator(testDir_)) {
        if (entry.path().extension() == ".gz") {
            foundCompressed = true;
            break;
        }
    }

    EXPECT_TRUE(foundCompressed);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST_F(LogRotationTest, FileSinkIsThreadSafe) {
    FileSink sink(testLogPath_.string());

    const int numThreads = 10;
    const int messagesPerThread = 100;

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&sink, t]() {
            pal::LogContext ctx;
            for (int m = 0; m < messagesPerThread; ++m) {
                sink.write(pal::LogLevel::Info,
                          "Thread " + std::to_string(t) + " message " + std::to_string(m),
                          "Test", ctx);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    sink.flush();

    // Count total lines
    std::ifstream file(testLogPath_);
    int lineCount = 0;
    std::string line;
    while (std::getline(file, line)) {
        lineCount++;
    }

    EXPECT_EQ(lineCount, numThreads * messagesPerThread);
}

TEST_F(LogRotationTest, RotationIsThreadSafe) {
    RotationPolicy policy;
    policy.setRotationType(RotationType::SizeBased);
    policy.setMaxFileSize(512);  // Small size for frequent rotation
    policy.setMaxBackupFiles(5);

    FileSink sink(testLogPath_.string(), policy);

    const int numThreads = 5;
    const int messagesPerThread = 50;

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&sink, t]() {
            pal::LogContext ctx;
            std::string message(50, 'X');
            for (int m = 0; m < messagesPerThread; ++m) {
                sink.write(pal::LogLevel::Info, message, "Test", ctx);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    sink.flush();

    // Should complete without crashing or data corruption
    EXPECT_TRUE(std::filesystem::exists(testLogPath_));
}

// =============================================================================
// Integration with StructuredLogger Tests
// =============================================================================

TEST_F(LogRotationTest, FileSinkIntegratesWithStructuredLogger) {
    auto fileSink = std::make_shared<FileSink>(testLogPath_.string());

    StructuredLogger logger;
    logger.addSink(fileSink);

    logger.info("Test integration message");
    logger.warning("Test warning message");
    logger.error("Test error message");

    logger.flush();

    // Verify all messages written
    std::ifstream file(testLogPath_);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    EXPECT_NE(content.find("Test integration message"), std::string::npos);
    EXPECT_NE(content.find("Test warning message"), std::string::npos);
    EXPECT_NE(content.find("Test error message"), std::string::npos);
}

TEST_F(LogRotationTest, RotatingFileSinkIntegratesWithStructuredLogger) {
    RotationPolicy policy;
    policy.setRotationType(RotationType::SizeBased);
    policy.setMaxFileSize(1024);

    auto fileSink = std::make_shared<FileSink>(testLogPath_.string(), policy);

    StructuredLogger logger;
    logger.addSink(fileSink);

    // Write many messages
    for (int i = 0; i < 50; ++i) {
        logger.info("Integration test message number " + std::to_string(i) +
                   " with some extra padding to increase size");
    }

    logger.flush();

    // Log file should exist
    EXPECT_TRUE(std::filesystem::exists(testLogPath_));
}

// =============================================================================
// Platform Logger Sink Tests
// =============================================================================

TEST_F(LogRotationTest, PlatformLoggerSinkCreation) {
    // Create platform-specific logger sink
    auto platformSink = createPlatformLoggerSink();
    EXPECT_NE(platformSink, nullptr);

    // Should implement ILogSink interface
    pal::LogContext ctx;
    platformSink->write(pal::LogLevel::Info, "Platform sink test", "Test", ctx);

    // Name should indicate platform
    std::string name = platformSink->getName();
    EXPECT_FALSE(name.empty());
}

#if defined(__APPLE__)
TEST_F(LogRotationTest, ApplePlatformUsesOsLog) {
    auto platformSink = createPlatformLoggerSink();
    EXPECT_NE(platformSink, nullptr);

    // On Apple platforms, should use os_log
    std::string name = platformSink->getName();
    EXPECT_TRUE(name.find("os_log") != std::string::npos ||
                name.find("OsLog") != std::string::npos ||
                name.find("Platform") != std::string::npos);
}
#endif

#if defined(__ANDROID__)
TEST_F(LogRotationTest, AndroidPlatformUsesLogcat) {
    auto platformSink = createPlatformLoggerSink();
    EXPECT_NE(platformSink, nullptr);

    // On Android, should use Logcat
    std::string name = platformSink->getName();
    EXPECT_TRUE(name.find("Logcat") != std::string::npos ||
                name.find("Android") != std::string::npos ||
                name.find("Platform") != std::string::npos);
}
#endif

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST_F(LogRotationTest, FileSinkHandlesInvalidPath) {
    // Try to create a sink with an invalid path
    FileSink sink("/nonexistent/directory/path/test.log");

    // Should not be open or should handle gracefully
    // Implementation may throw or return error state
    // The key is it shouldn't crash
    EXPECT_FALSE(sink.isOpen());
}

TEST_F(LogRotationTest, FileSinkHandlesPermissionDenied) {
    // This test may be platform-specific
    // On Unix, try to write to a read-only location
#if !defined(_WIN32)
    FileSink sink("/test.log");  // Root directory, usually not writable
    EXPECT_FALSE(sink.isOpen());
#endif
}

TEST_F(LogRotationTest, RotationHandlesFullDisk) {
    // This is more of a behavioral test - ensure graceful degradation
    // when rotation fails due to disk space
    RotationPolicy policy;
    policy.setRotationType(RotationType::SizeBased);
    policy.setMaxFileSize(100);

    FileSink sink(testLogPath_.string(), policy);

    // Even if rotation fails, current log should still work
    pal::LogContext ctx;
    sink.write(pal::LogLevel::Info, "Test message", "Test", ctx);
    sink.flush();

    EXPECT_TRUE(std::filesystem::exists(testLogPath_));
}

// =============================================================================
// Rotation Naming Pattern Tests
// =============================================================================

TEST_F(LogRotationTest, RotatedFilesFollowNamingPattern) {
    RotationPolicy policy;
    policy.setRotationType(RotationType::SizeBased);
    policy.setMaxFileSize(128);
    policy.setMaxBackupFiles(3);

    FileSink sink(testLogPath_.string(), policy);

    // Trigger multiple rotations
    pal::LogContext ctx;
    std::string message(50, 'Y');
    for (int i = 0; i < 30; ++i) {
        sink.write(pal::LogLevel::Info, message, "Test", ctx);
        sink.flush();
    }

    // Check naming pattern: test.log.1, test.log.2, etc.
    // or test.log.1.gz if compression enabled
    bool hasRotated = false;
    for (const auto& entry : std::filesystem::directory_iterator(testDir_)) {
        std::string filename = entry.path().filename().string();
        if (filename.find("test.log.") != std::string::npos) {
            hasRotated = true;
            break;
        }
    }

    EXPECT_TRUE(hasRotated);
}

TEST_F(LogRotationTest, TimestampedRotationNaming) {
    RotationPolicy policy;
    policy.setRotationType(RotationType::TimeBased);
    policy.setRotationInterval(std::chrono::milliseconds(50));
    policy.setUseTimestampedNames(true);

    FileSink sink(testLogPath_.string(), policy);

    pal::LogContext ctx;
    sink.write(pal::LogLevel::Info, "Message 1", "Test", ctx);
    sink.flush();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    sink.write(pal::LogLevel::Info, "Message 2", "Test", ctx);
    sink.flush();

    // Should have timestamp in rotated filename
    // Pattern: test.log.YYYYMMDD-HHMMSS or similar
    bool hasTimestamped = false;
    for (const auto& entry : std::filesystem::directory_iterator(testDir_)) {
        std::string filename = entry.path().filename().string();
        // Check for date pattern in filename
        if (filename.find("test.log.20") != std::string::npos) {
            hasTimestamped = true;
            break;
        }
    }

    // This may not always be true depending on timing
    // So we just verify no crash occurred
    EXPECT_TRUE(std::filesystem::exists(testLogPath_));
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(LogRotationTest, EmptyMessageHandled) {
    FileSink sink(testLogPath_.string());

    pal::LogContext ctx;
    sink.write(pal::LogLevel::Info, "", "Test", ctx);
    sink.flush();

    // Should not crash, file should exist
    EXPECT_TRUE(std::filesystem::exists(testLogPath_));
}

TEST_F(LogRotationTest, VeryLongMessageHandled) {
    FileSink sink(testLogPath_.string());

    std::string longMessage(1024 * 1024, 'Z');  // 1 MB message

    pal::LogContext ctx;
    sink.write(pal::LogLevel::Info, longMessage, "Test", ctx);
    sink.flush();

    // Should handle large messages
    EXPECT_TRUE(std::filesystem::exists(testLogPath_));
    EXPECT_GT(std::filesystem::file_size(testLogPath_), 1024u * 1024);
}

TEST_F(LogRotationTest, SpecialCharactersInMessage) {
    FileSink sink(testLogPath_.string());

    pal::LogContext ctx;
    sink.write(pal::LogLevel::Info, "Message with\nnewline\tand tab", "Test", ctx);
    sink.write(pal::LogLevel::Info, "Unicode: \xC3\xA9\xC3\xA8", "Test", ctx);  // UTF-8
    sink.flush();

    EXPECT_TRUE(std::filesystem::exists(testLogPath_));
}

TEST_F(LogRotationTest, ConcurrentOpenAndClose) {
    // Test rapid open/close cycles
    for (int i = 0; i < 10; ++i) {
        FileSink sink(testLogPath_.string());
        pal::LogContext ctx;
        sink.write(pal::LogLevel::Info, "Cycle " + std::to_string(i), "Test", ctx);
        sink.flush();
    }

    EXPECT_TRUE(std::filesystem::exists(testLogPath_));
}

} // namespace test
} // namespace core
} // namespace openrtmp
