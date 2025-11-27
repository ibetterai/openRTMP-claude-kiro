// OpenRTMP - Cross-platform RTMP Server
// Tests for Linux/Android Log PAL Implementation
//
// Requirements Covered: 18.5 (Platform-native logging integration - Logcat on Android)

#include <gtest/gtest.h>
#include "openrtmp/pal/log_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"

#include <atomic>
#include <vector>
#include <memory>

#if defined(__linux__) || defined(__ANDROID__)
#include "openrtmp/pal/linux/linux_log_pal.hpp"
#endif

namespace openrtmp {
namespace pal {
namespace test {

#if defined(__linux__) || defined(__ANDROID__)

// =============================================================================
// Test Log Sink
// =============================================================================

class TestLogSink : public ILogSink {
public:
    void write(
        LogLevel level,
        const std::string& message,
        const std::string& category,
        const LogContext& context
    ) override {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.push_back({level, message, category});
    }

    void flush() override {
        // No-op for test sink
    }

    std::string getName() const override {
        return "TestLogSink";
    }

    struct LogEntry {
        LogLevel level;
        std::string message;
        std::string category;
    };

    std::vector<LogEntry> getMessages() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return messages_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.clear();
    }

    size_t messageCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return messages_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<LogEntry> messages_;
};

// =============================================================================
// Linux Log PAL Tests
// =============================================================================

class LinuxLogPALTest : public ::testing::Test {
protected:
    void SetUp() override {
        logPal_ = std::make_unique<linux::LinuxLogPAL>();
        testSink_ = std::make_shared<TestLogSink>();
        logPal_->addSink(testSink_);
    }

    void TearDown() override {
        logPal_->removeSink(testSink_);
        testSink_.reset();
        logPal_.reset();
    }

    std::unique_ptr<linux::LinuxLogPAL> logPal_;
    std::shared_ptr<TestLogSink> testSink_;
};

TEST_F(LinuxLogPALTest, ImplementsILogPALInterface) {
    ILogPAL* interface = logPal_.get();
    EXPECT_NE(interface, nullptr);
}

// =============================================================================
// Basic Logging Tests
// =============================================================================

TEST_F(LinuxLogPALTest, LogMessageIsDeliveredToSink) {
    LogContext ctx{"test.cpp", 42, "testFunction"};
    logPal_->log(LogLevel::Info, "Test message", "Test", ctx);

    EXPECT_EQ(testSink_->messageCount(), 1);

    auto messages = testSink_->getMessages();
    EXPECT_EQ(messages[0].level, LogLevel::Info);
    EXPECT_EQ(messages[0].message, "Test message");
    EXPECT_EQ(messages[0].category, "Test");
}

TEST_F(LinuxLogPALTest, LogAllLevels) {
    LogContext ctx{"test.cpp", 1, "test"};

    logPal_->setMinLevel(LogLevel::Trace);  // Enable all levels

    logPal_->log(LogLevel::Trace, "Trace message", "Test", ctx);
    logPal_->log(LogLevel::Debug, "Debug message", "Test", ctx);
    logPal_->log(LogLevel::Info, "Info message", "Test", ctx);
    logPal_->log(LogLevel::Warning, "Warning message", "Test", ctx);
    logPal_->log(LogLevel::Error, "Error message", "Test", ctx);
    logPal_->log(LogLevel::Critical, "Critical message", "Test", ctx);

    EXPECT_EQ(testSink_->messageCount(), 6);
}

TEST_F(LinuxLogPALTest, LogWithDifferentCategories) {
    LogContext ctx{"test.cpp", 1, "test"};

    logPal_->log(LogLevel::Info, "Network message", "Network", ctx);
    logPal_->log(LogLevel::Info, "Protocol message", "Protocol", ctx);
    logPal_->log(LogLevel::Info, "Server message", "Server", ctx);

    EXPECT_EQ(testSink_->messageCount(), 3);

    auto messages = testSink_->getMessages();
    EXPECT_EQ(messages[0].category, "Network");
    EXPECT_EQ(messages[1].category, "Protocol");
    EXPECT_EQ(messages[2].category, "Server");
}

// =============================================================================
// Log Level Filtering Tests
// =============================================================================

TEST_F(LinuxLogPALTest, SetMinLevelFiltersLowerLevels) {
    LogContext ctx{"test.cpp", 1, "test"};

    logPal_->setMinLevel(LogLevel::Warning);

    logPal_->log(LogLevel::Trace, "Should not appear", "Test", ctx);
    logPal_->log(LogLevel::Debug, "Should not appear", "Test", ctx);
    logPal_->log(LogLevel::Info, "Should not appear", "Test", ctx);
    logPal_->log(LogLevel::Warning, "Warning appears", "Test", ctx);
    logPal_->log(LogLevel::Error, "Error appears", "Test", ctx);
    logPal_->log(LogLevel::Critical, "Critical appears", "Test", ctx);

    EXPECT_EQ(testSink_->messageCount(), 3);
}

TEST_F(LinuxLogPALTest, GetMinLevelReturnsSetValue) {
    logPal_->setMinLevel(LogLevel::Error);
    EXPECT_EQ(logPal_->getMinLevel(), LogLevel::Error);

    logPal_->setMinLevel(LogLevel::Trace);
    EXPECT_EQ(logPal_->getMinLevel(), LogLevel::Trace);
}

TEST_F(LinuxLogPALTest, DefaultMinLevelIsDebugOrInfo) {
    linux::LinuxLogPAL freshPal;
    LogLevel minLevel = freshPal.getMinLevel();

    // Should be Debug or Info by default (implementation-specific)
    EXPECT_TRUE(minLevel == LogLevel::Debug || minLevel == LogLevel::Info);
}

TEST_F(LinuxLogPALTest, MinLevelOffDisablesAllLogging) {
    LogContext ctx{"test.cpp", 1, "test"};

    logPal_->setMinLevel(LogLevel::Off);

    logPal_->log(LogLevel::Trace, "Should not appear", "Test", ctx);
    logPal_->log(LogLevel::Debug, "Should not appear", "Test", ctx);
    logPal_->log(LogLevel::Info, "Should not appear", "Test", ctx);
    logPal_->log(LogLevel::Warning, "Should not appear", "Test", ctx);
    logPal_->log(LogLevel::Error, "Should not appear", "Test", ctx);
    logPal_->log(LogLevel::Critical, "Should not appear", "Test", ctx);

    EXPECT_EQ(testSink_->messageCount(), 0);
}

// =============================================================================
// Sink Management Tests
// =============================================================================

TEST_F(LinuxLogPALTest, AddMultipleSinks) {
    auto secondSink = std::make_shared<TestLogSink>();
    logPal_->addSink(secondSink);

    LogContext ctx{"test.cpp", 1, "test"};
    logPal_->log(LogLevel::Info, "Message to both sinks", "Test", ctx);

    EXPECT_EQ(testSink_->messageCount(), 1);
    EXPECT_EQ(secondSink->messageCount(), 1);

    logPal_->removeSink(secondSink);
}

TEST_F(LinuxLogPALTest, RemoveSinkStopsDelivery) {
    LogContext ctx{"test.cpp", 1, "test"};

    logPal_->log(LogLevel::Info, "Before removal", "Test", ctx);
    EXPECT_EQ(testSink_->messageCount(), 1);

    logPal_->removeSink(testSink_);

    logPal_->log(LogLevel::Info, "After removal", "Test", ctx);
    EXPECT_EQ(testSink_->messageCount(), 1);  // Still 1, no new message
}

TEST_F(LinuxLogPALTest, RemoveNonExistentSinkDoesNotCrash) {
    auto otherSink = std::make_shared<TestLogSink>();
    EXPECT_NO_THROW(logPal_->removeSink(otherSink));
}

// =============================================================================
// Flush Tests
// =============================================================================

TEST_F(LinuxLogPALTest, FlushCallsSinkFlush) {
    // Just ensure flush doesn't crash
    EXPECT_NO_THROW(logPal_->flush());
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST_F(LinuxLogPALTest, ConcurrentLoggingIsThreadSafe) {
    LogContext ctx{"test.cpp", 1, "test"};

    std::vector<std::thread> threads;
    const int numThreads = 10;
    const int messagesPerThread = 100;

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, &ctx, i, messagesPerThread]() {
            for (int j = 0; j < messagesPerThread; ++j) {
                logPal_->log(
                    LogLevel::Info,
                    "Thread " + std::to_string(i) + " message " + std::to_string(j),
                    "Test",
                    ctx
                );
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(testSink_->messageCount(), numThreads * messagesPerThread);
}

TEST_F(LinuxLogPALTest, ConcurrentLevelChangesAreThreadSafe) {
    LogContext ctx{"test.cpp", 1, "test"};

    std::atomic<bool> running{true};

    // Thread changing levels
    std::thread levelChanger([this, &running]() {
        while (running) {
            logPal_->setMinLevel(LogLevel::Trace);
            logPal_->setMinLevel(LogLevel::Error);
            logPal_->setMinLevel(LogLevel::Info);
        }
    });

    // Thread logging
    std::thread logger([this, &ctx, &running]() {
        for (int i = 0; i < 1000; ++i) {
            logPal_->log(LogLevel::Info, "Concurrent message", "Test", ctx);
        }
        running = false;
    });

    logger.join();
    levelChanger.join();

    // Just ensure we didn't crash; message count varies due to level changes
}

// =============================================================================
// Log Context Tests
// =============================================================================

TEST_F(LinuxLogPALTest, LogContextIsPreserved) {
    LogContext ctx;
    ctx.file = "myfile.cpp";
    ctx.line = 123;
    ctx.function = "myFunction";
    ctx.threadName = "TestThread";

    // Log context is passed to sink but not directly accessible in test sink
    // This test just ensures the context is accepted without errors
    EXPECT_NO_THROW(logPal_->log(LogLevel::Info, "Context test", "Test", ctx));
    EXPECT_EQ(testSink_->messageCount(), 1);
}

// =============================================================================
// Convenience Macro Tests
// =============================================================================

TEST_F(LinuxLogPALTest, LogMacrosWork) {
    // Test that the macros compile and work
    OPENRTMP_LOG_INFO(logPal_.get(), "Test", "Info via macro");
    EXPECT_EQ(testSink_->messageCount(), 1);
}

#ifdef __ANDROID__
// =============================================================================
// Android-Specific Logcat Tests
// =============================================================================

TEST_F(LinuxLogPALTest, LogcatIntegrationDoesNotCrash) {
    // On Android, messages should also be sent to Logcat
    // This test just ensures the Logcat integration doesn't crash
    LogContext ctx{"test.cpp", 1, "test"};

    EXPECT_NO_THROW(logPal_->log(LogLevel::Info, "Logcat test message", "OpenRTMP", ctx));
    EXPECT_NO_THROW(logPal_->log(LogLevel::Error, "Logcat error message", "OpenRTMP", ctx));
    EXPECT_NO_THROW(logPal_->log(LogLevel::Debug, "Logcat debug message", "OpenRTMP", ctx));
}
#endif

#endif // defined(__linux__) || defined(__ANDROID__)

} // namespace test
} // namespace pal
} // namespace openrtmp
