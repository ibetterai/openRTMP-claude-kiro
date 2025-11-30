// OpenRTMP - Cross-platform RTMP Server
// Tests for Structured Logging Component
//
// Requirements Covered:
// - 18.1: Support configurable log levels (debug, info, warning, error)
// - 18.2: Log all connection events with timestamps and client details
// - 18.6: Support JSON structured log format for aggregation systems
// - 18.7: Include context in error logs (stream keys, client IPs, error codes)

#include <gtest/gtest.h>
#include "openrtmp/core/structured_logger.hpp"
#include "openrtmp/pal/log_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"

#include <chrono>
#include <thread>
#include <vector>
#include <regex>
#include <sstream>

namespace openrtmp {
namespace core {
namespace test {

// =============================================================================
// Test Sink for Capturing Log Output
// =============================================================================

class TestLogSink : public pal::ILogSink {
public:
    void write(pal::LogLevel level, const std::string& message,
               const std::string& category, const pal::LogContext& context) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry entry;
        entry.level = level;
        entry.message = message;
        entry.category = category;
        entry.context = context;
        entries_.push_back(entry);
    }

    void flush() override {
        flushCount_++;
    }

    std::string getName() const override {
        return "TestLogSink";
    }

    struct Entry {
        pal::LogLevel level;
        std::string message;
        std::string category;
        pal::LogContext context;
    };

    std::vector<Entry> getEntries() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

    int getFlushCount() const {
        return flushCount_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    int flushCount_ = 0;
};

// =============================================================================
// StructuredLogger Basic Tests
// =============================================================================

class StructuredLoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        testSink_ = std::make_shared<TestLogSink>();
        logger_ = std::make_unique<StructuredLogger>();
        logger_->addSink(testSink_);
    }

    void TearDown() override {
        logger_.reset();
        testSink_.reset();
    }

    std::unique_ptr<StructuredLogger> logger_;
    std::shared_ptr<TestLogSink> testSink_;
};

// Test: Requirement 18.1 - Configurable log levels
TEST_F(StructuredLoggerTest, SupportsConfigurableLogLevels) {
    // Test that all four required log levels are supported
    logger_->setLevel(LogLevelConfig::Debug);
    EXPECT_EQ(logger_->getLevel(), LogLevelConfig::Debug);

    logger_->setLevel(LogLevelConfig::Info);
    EXPECT_EQ(logger_->getLevel(), LogLevelConfig::Info);

    logger_->setLevel(LogLevelConfig::Warning);
    EXPECT_EQ(logger_->getLevel(), LogLevelConfig::Warning);

    logger_->setLevel(LogLevelConfig::Error);
    EXPECT_EQ(logger_->getLevel(), LogLevelConfig::Error);
}

TEST_F(StructuredLoggerTest, FiltersMessagesBelowConfiguredLevel) {
    logger_->setLevel(LogLevelConfig::Warning);

    // Debug and Info should be filtered
    logger_->debug("debug message");
    logger_->info("info message");
    // Warning and Error should pass
    logger_->warning("warning message");
    logger_->error("error message");

    auto entries = testSink_->getEntries();
    EXPECT_EQ(entries.size(), 2u);
    // Formatted messages contain the original message plus metadata
    EXPECT_NE(entries[0].message.find("warning message"), std::string::npos);
    EXPECT_NE(entries[1].message.find("error message"), std::string::npos);
}

TEST_F(StructuredLoggerTest, DebugLevelAllowsAllMessages) {
    logger_->setLevel(LogLevelConfig::Debug);

    logger_->debug("debug");
    logger_->info("info");
    logger_->warning("warning");
    logger_->error("error");

    EXPECT_EQ(testSink_->size(), 4u);
}

// Test: Requirement 18.2 - Timestamps in logs
TEST_F(StructuredLoggerTest, LogsIncludeTimestamps) {
    logger_->setJsonFormat(true);
    logger_->info("test message");

    auto entries = testSink_->getEntries();
    ASSERT_EQ(entries.size(), 1u);

    // JSON format should include timestamp field
    EXPECT_NE(entries[0].message.find("\"timestamp\""), std::string::npos);
}

TEST_F(StructuredLoggerTest, TimestampIsISO8601Format) {
    logger_->setJsonFormat(true);
    logger_->info("test message");

    auto entries = testSink_->getEntries();
    ASSERT_EQ(entries.size(), 1u);

    // ISO 8601 format: YYYY-MM-DDTHH:MM:SS.sssZ or similar
    std::regex iso8601Regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})");
    EXPECT_TRUE(std::regex_search(entries[0].message, iso8601Regex));
}

// Test: Requirement 18.2 - Connection events with client details
TEST_F(StructuredLoggerTest, LogConnectionEventWithClientDetails) {
    ClientInfo client;
    client.ip = "192.168.1.100";
    client.port = 54321;
    client.userAgent = "OBS/29.0";

    logger_->setJsonFormat(true);
    logger_->logConnectionEvent(ConnectionEventType::Connected, client);

    auto entries = testSink_->getEntries();
    ASSERT_EQ(entries.size(), 1u);

    // Should include client IP, port
    EXPECT_NE(entries[0].message.find("192.168.1.100"), std::string::npos);
    EXPECT_NE(entries[0].message.find("54321"), std::string::npos);
}

TEST_F(StructuredLoggerTest, LogsAllConnectionEventTypes) {
    ClientInfo client;
    client.ip = "10.0.0.1";
    client.port = 1935;

    logger_->logConnectionEvent(ConnectionEventType::Connected, client);
    logger_->logConnectionEvent(ConnectionEventType::Disconnected, client);
    logger_->logConnectionEvent(ConnectionEventType::PublishStart, client);
    logger_->logConnectionEvent(ConnectionEventType::PublishStop, client);
    logger_->logConnectionEvent(ConnectionEventType::PlayStart, client);
    logger_->logConnectionEvent(ConnectionEventType::PlayStop, client);

    EXPECT_EQ(testSink_->size(), 6u);
}

// Test: Requirement 18.6 - JSON structured log format
TEST_F(StructuredLoggerTest, SupportsJsonFormat) {
    logger_->setJsonFormat(true);
    EXPECT_TRUE(logger_->isJsonFormat());

    logger_->setJsonFormat(false);
    EXPECT_FALSE(logger_->isJsonFormat());
}

TEST_F(StructuredLoggerTest, JsonFormatProducesValidJson) {
    logger_->setJsonFormat(true);
    logger_->info("test message");

    auto entries = testSink_->getEntries();
    ASSERT_EQ(entries.size(), 1u);

    // Should start and end with braces
    EXPECT_EQ(entries[0].message.front(), '{');
    EXPECT_EQ(entries[0].message.back(), '}');

    // Should contain required fields
    EXPECT_NE(entries[0].message.find("\"level\""), std::string::npos);
    EXPECT_NE(entries[0].message.find("\"message\""), std::string::npos);
    EXPECT_NE(entries[0].message.find("\"timestamp\""), std::string::npos);
}

TEST_F(StructuredLoggerTest, JsonFormatEscapesSpecialCharacters) {
    logger_->setJsonFormat(true);
    logger_->info("message with \"quotes\" and \\ backslash");

    auto entries = testSink_->getEntries();
    ASSERT_EQ(entries.size(), 1u);

    // Quotes and backslashes should be escaped in JSON
    EXPECT_NE(entries[0].message.find("\\\"quotes\\\""), std::string::npos);
    EXPECT_NE(entries[0].message.find("\\\\"), std::string::npos);
}

TEST_F(StructuredLoggerTest, NonJsonFormatIsReadable) {
    logger_->setJsonFormat(false);
    logger_->info("test message");

    auto entries = testSink_->getEntries();
    ASSERT_EQ(entries.size(), 1u);

    // Non-JSON format should be human-readable
    EXPECT_NE(entries[0].message.find("test message"), std::string::npos);
    // Should not be JSON
    EXPECT_NE(entries[0].message.front(), '{');
}

// Test: Requirement 18.7 - Context in error logs
TEST_F(StructuredLoggerTest, ErrorLogsIncludeStreamKey) {
    logger_->setJsonFormat(true);

    LogContext ctx;
    ctx.streamKey = "live/mystream";
    logger_->errorWithContext("Stream error", ctx);

    auto entries = testSink_->getEntries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_NE(entries[0].message.find("live/mystream"), std::string::npos);
}

TEST_F(StructuredLoggerTest, ErrorLogsIncludeClientIP) {
    logger_->setJsonFormat(true);

    LogContext ctx;
    ctx.clientIP = "192.168.1.50";
    logger_->errorWithContext("Connection error", ctx);

    auto entries = testSink_->getEntries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_NE(entries[0].message.find("192.168.1.50"), std::string::npos);
}

TEST_F(StructuredLoggerTest, ErrorLogsIncludeErrorCode) {
    logger_->setJsonFormat(true);

    LogContext ctx;
    ctx.errorCode = 1001;
    logger_->errorWithContext("Operation failed", ctx);

    auto entries = testSink_->getEntries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_NE(entries[0].message.find("1001"), std::string::npos);
}

TEST_F(StructuredLoggerTest, ErrorLogsIncludeSessionId) {
    logger_->setJsonFormat(true);

    LogContext ctx;
    ctx.sessionId = 12345678;
    logger_->errorWithContext("Session error", ctx);

    auto entries = testSink_->getEntries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_NE(entries[0].message.find("12345678"), std::string::npos);
}

TEST_F(StructuredLoggerTest, ErrorLogsIncludeAllContextFields) {
    logger_->setJsonFormat(true);

    LogContext ctx;
    ctx.streamKey = "live/broadcast";
    ctx.clientIP = "10.20.30.40";
    ctx.errorCode = 2002;
    ctx.sessionId = 99887766;
    logger_->errorWithContext("Full context error", ctx);

    auto entries = testSink_->getEntries();
    ASSERT_EQ(entries.size(), 1u);

    const std::string& msg = entries[0].message;
    EXPECT_NE(msg.find("live/broadcast"), std::string::npos);
    EXPECT_NE(msg.find("10.20.30.40"), std::string::npos);
    EXPECT_NE(msg.find("2002"), std::string::npos);
    EXPECT_NE(msg.find("99887766"), std::string::npos);
}

// Test: Thread safety
TEST_F(StructuredLoggerTest, ThreadSafeLogging) {
    const int numThreads = 10;
    const int messagesPerThread = 100;

    std::vector<std::thread> threads;
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, i]() {
            for (int j = 0; j < 100; ++j) {
                logger_->info("Thread " + std::to_string(i) + " message " + std::to_string(j));
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(testSink_->size(), static_cast<size_t>(numThreads * messagesPerThread));
}

// Test: Sink management
TEST_F(StructuredLoggerTest, AddAndRemoveSinks) {
    auto sink2 = std::make_shared<TestLogSink>();
    logger_->addSink(sink2);

    logger_->info("broadcast to both sinks");

    EXPECT_EQ(testSink_->size(), 1u);
    EXPECT_EQ(sink2->size(), 1u);

    logger_->removeSink(sink2);
    logger_->info("only to first sink");

    EXPECT_EQ(testSink_->size(), 2u);
    EXPECT_EQ(sink2->size(), 1u);
}

TEST_F(StructuredLoggerTest, FlushForwardsToSinks) {
    logger_->flush();
    EXPECT_EQ(testSink_->getFlushCount(), 1);
}

// Test: Category support
TEST_F(StructuredLoggerTest, SupportsCategories) {
    logger_->info("Network message", "Network");
    logger_->info("Protocol message", "Protocol");

    auto entries = testSink_->getEntries();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].category, "Network");
    EXPECT_EQ(entries[1].category, "Protocol");
}

// Test: Default category
TEST_F(StructuredLoggerTest, UsesDefaultCategoryWhenNotSpecified) {
    logger_->info("message without category");

    auto entries = testSink_->getEntries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].category, "OpenRTMP");  // Default category
}

// =============================================================================
// LogContext Tests
// =============================================================================

TEST_F(StructuredLoggerTest, LogContextDefaultsAreEmpty) {
    LogContext ctx;
    EXPECT_TRUE(ctx.streamKey.empty());
    EXPECT_TRUE(ctx.clientIP.empty());
    EXPECT_EQ(ctx.errorCode, 0);
    EXPECT_EQ(ctx.sessionId, 0);
}

// =============================================================================
// Connection Event Type Tests
// =============================================================================

TEST_F(StructuredLoggerTest, ConnectionEventTypeToString) {
    EXPECT_EQ(connectionEventTypeToString(ConnectionEventType::Connected), "connected");
    EXPECT_EQ(connectionEventTypeToString(ConnectionEventType::Disconnected), "disconnected");
    EXPECT_EQ(connectionEventTypeToString(ConnectionEventType::PublishStart), "publish_start");
    EXPECT_EQ(connectionEventTypeToString(ConnectionEventType::PublishStop), "publish_stop");
    EXPECT_EQ(connectionEventTypeToString(ConnectionEventType::PlayStart), "play_start");
    EXPECT_EQ(connectionEventTypeToString(ConnectionEventType::PlayStop), "play_stop");
}

// =============================================================================
// JSON Escaping Tests
// =============================================================================

TEST_F(StructuredLoggerTest, JsonEscapesNewlines) {
    logger_->setJsonFormat(true);
    logger_->info("line1\nline2");

    auto entries = testSink_->getEntries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_NE(entries[0].message.find("\\n"), std::string::npos);
}

TEST_F(StructuredLoggerTest, JsonEscapesTabs) {
    logger_->setJsonFormat(true);
    logger_->info("col1\tcol2");

    auto entries = testSink_->getEntries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_NE(entries[0].message.find("\\t"), std::string::npos);
}

// =============================================================================
// Platform Integration Tests
// =============================================================================

TEST_F(StructuredLoggerTest, CanCreateWithPlatformLogger) {
    // Test that StructuredLogger can be created with a platform-specific PAL
    // This verifies the platform integration architecture
    auto platformLogger = std::make_shared<TestLogSink>();
    StructuredLogger loggerWithPlatform;
    loggerWithPlatform.addSink(platformLogger);

    loggerWithPlatform.info("test with platform sink");
    EXPECT_EQ(platformLogger->size(), 1u);
}

// =============================================================================
// Log Level String Conversion Tests
// =============================================================================

TEST_F(StructuredLoggerTest, LogLevelToString) {
    EXPECT_EQ(logLevelToString(LogLevelConfig::Debug), "debug");
    EXPECT_EQ(logLevelToString(LogLevelConfig::Info), "info");
    EXPECT_EQ(logLevelToString(LogLevelConfig::Warning), "warning");
    EXPECT_EQ(logLevelToString(LogLevelConfig::Error), "error");
}

TEST_F(StructuredLoggerTest, StringToLogLevel) {
    EXPECT_EQ(stringToLogLevel("debug"), LogLevelConfig::Debug);
    EXPECT_EQ(stringToLogLevel("info"), LogLevelConfig::Info);
    EXPECT_EQ(stringToLogLevel("warning"), LogLevelConfig::Warning);
    EXPECT_EQ(stringToLogLevel("error"), LogLevelConfig::Error);

    // Case insensitive
    EXPECT_EQ(stringToLogLevel("DEBUG"), LogLevelConfig::Debug);
    EXPECT_EQ(stringToLogLevel("Info"), LogLevelConfig::Info);

    // Unknown defaults to Info
    EXPECT_EQ(stringToLogLevel("unknown"), LogLevelConfig::Info);
}

} // namespace test
} // namespace core
} // namespace openrtmp
