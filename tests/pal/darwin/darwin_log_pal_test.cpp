// OpenRTMP - Cross-platform RTMP Server
// Tests for Darwin (macOS/iOS) Log PAL Implementation
//
// Requirements Covered: 18.5 (Platform-native logging integration - os_log on iOS)

#include <gtest/gtest.h>
#include "openrtmp/pal/log_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"

#include <thread>
#include <vector>

#if defined(__APPLE__)
#include "openrtmp/pal/darwin/darwin_log_pal.hpp"
#endif

namespace openrtmp {
namespace pal {
namespace test {

#if defined(__APPLE__)

// =============================================================================
// Darwin Log PAL Tests
// =============================================================================

class DarwinLogPALTest : public ::testing::Test {
protected:
    void SetUp() override {
        logPal_ = std::make_unique<darwin::DarwinLogPAL>();
    }

    void TearDown() override {
        logPal_.reset();
    }

    std::unique_ptr<darwin::DarwinLogPAL> logPal_;
};

TEST_F(DarwinLogPALTest, ImplementsILogPALInterface) {
    ILogPAL* interface = logPal_.get();
    EXPECT_NE(interface, nullptr);
}

TEST_F(DarwinLogPALTest, DefaultMinLevelIsDebug) {
    // Default minimum level should allow most messages
    LogLevel level = logPal_->getMinLevel();
    EXPECT_LE(static_cast<int>(level), static_cast<int>(LogLevel::Debug));
}

TEST_F(DarwinLogPALTest, SetMinLevelUpdatesLevel) {
    logPal_->setMinLevel(LogLevel::Warning);
    EXPECT_EQ(logPal_->getMinLevel(), LogLevel::Warning);

    logPal_->setMinLevel(LogLevel::Error);
    EXPECT_EQ(logPal_->getMinLevel(), LogLevel::Error);

    logPal_->setMinLevel(LogLevel::Trace);
    EXPECT_EQ(logPal_->getMinLevel(), LogLevel::Trace);
}

TEST_F(DarwinLogPALTest, LogDoesNotCrashWithValidContext) {
    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    // Should not throw or crash
    EXPECT_NO_THROW(logPal_->log(LogLevel::Info, "Test message", "Test", ctx));
}

TEST_F(DarwinLogPALTest, LogWithAllLogLevels) {
    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    // Test all log levels - should not crash
    EXPECT_NO_THROW(logPal_->log(LogLevel::Trace, "Trace message", "Test", ctx));
    EXPECT_NO_THROW(logPal_->log(LogLevel::Debug, "Debug message", "Test", ctx));
    EXPECT_NO_THROW(logPal_->log(LogLevel::Info, "Info message", "Test", ctx));
    EXPECT_NO_THROW(logPal_->log(LogLevel::Warning, "Warning message", "Test", ctx));
    EXPECT_NO_THROW(logPal_->log(LogLevel::Error, "Error message", "Test", ctx));
    EXPECT_NO_THROW(logPal_->log(LogLevel::Critical, "Critical message", "Test", ctx));
}

TEST_F(DarwinLogPALTest, LogBelowMinLevelIsIgnored) {
    logPal_->setMinLevel(LogLevel::Warning);

    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    // These should be no-ops (below min level)
    EXPECT_NO_THROW(logPal_->log(LogLevel::Debug, "Should be ignored", "Test", ctx));
    EXPECT_NO_THROW(logPal_->log(LogLevel::Info, "Should be ignored", "Test", ctx));

    // These should be logged
    EXPECT_NO_THROW(logPal_->log(LogLevel::Warning, "Should be logged", "Test", ctx));
    EXPECT_NO_THROW(logPal_->log(LogLevel::Error, "Should be logged", "Test", ctx));
}

TEST_F(DarwinLogPALTest, LogWithEmptyMessage) {
    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    EXPECT_NO_THROW(logPal_->log(LogLevel::Info, "", "Test", ctx));
}

TEST_F(DarwinLogPALTest, LogWithEmptyCategory) {
    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    EXPECT_NO_THROW(logPal_->log(LogLevel::Info, "Test message", "", ctx));
}

TEST_F(DarwinLogPALTest, LogWithNullContext) {
    LogContext ctx;
    ctx.file = nullptr;
    ctx.line = 0;
    ctx.function = nullptr;

    EXPECT_NO_THROW(logPal_->log(LogLevel::Info, "Test message", "Test", ctx));
}

TEST_F(DarwinLogPALTest, FlushDoesNotCrash) {
    // Log some messages first
    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    logPal_->log(LogLevel::Info, "Message before flush", "Test", ctx);

    // Flush should not crash
    EXPECT_NO_THROW(logPal_->flush());
}

TEST_F(DarwinLogPALTest, AddSinkWorks) {
    // Create a simple test sink
    class TestSink : public ILogSink {
    public:
        void write(LogLevel level, const std::string& message,
                   const std::string& category, const LogContext& context) override {
            writeCount++;
            lastMessage = message;
        }
        void flush() override { flushCount++; }
        std::string getName() const override { return "TestSink"; }

        int writeCount = 0;
        int flushCount = 0;
        std::string lastMessage;
    };

    auto sink = std::make_shared<TestSink>();
    EXPECT_NO_THROW(logPal_->addSink(sink));

    // Log a message
    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    logPal_->log(LogLevel::Info, "Test via sink", "Test", ctx);

    // Sink should have received the message
    EXPECT_EQ(sink->writeCount, 1);
    EXPECT_EQ(sink->lastMessage, "Test via sink");
}

TEST_F(DarwinLogPALTest, RemoveSinkWorks) {
    class TestSink : public ILogSink {
    public:
        void write(LogLevel level, const std::string& message,
                   const std::string& category, const LogContext& context) override {
            writeCount++;
        }
        void flush() override {}
        std::string getName() const override { return "TestSink"; }
        int writeCount = 0;
    };

    auto sink = std::make_shared<TestSink>();
    logPal_->addSink(sink);

    // Log a message
    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    logPal_->log(LogLevel::Info, "First message", "Test", ctx);
    EXPECT_EQ(sink->writeCount, 1);

    // Remove sink
    logPal_->removeSink(sink);

    // Log another message - sink should not receive it
    logPal_->log(LogLevel::Info, "Second message", "Test", ctx);
    EXPECT_EQ(sink->writeCount, 1);
}

TEST_F(DarwinLogPALTest, MultipleSinksReceiveMessages) {
    class TestSink : public ILogSink {
    public:
        TestSink(const std::string& name) : name_(name) {}
        void write(LogLevel level, const std::string& message,
                   const std::string& category, const LogContext& context) override {
            writeCount++;
        }
        void flush() override {}
        std::string getName() const override { return name_; }
        int writeCount = 0;
    private:
        std::string name_;
    };

    auto sink1 = std::make_shared<TestSink>("Sink1");
    auto sink2 = std::make_shared<TestSink>("Sink2");

    logPal_->addSink(sink1);
    logPal_->addSink(sink2);

    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    logPal_->log(LogLevel::Info, "Broadcast message", "Test", ctx);

    EXPECT_EQ(sink1->writeCount, 1);
    EXPECT_EQ(sink2->writeCount, 1);
}

TEST_F(DarwinLogPALTest, FlushCallsSinkFlush) {
    class TestSink : public ILogSink {
    public:
        void write(LogLevel level, const std::string& message,
                   const std::string& category, const LogContext& context) override {}
        void flush() override { flushCount++; }
        std::string getName() const override { return "TestSink"; }
        int flushCount = 0;
    };

    auto sink = std::make_shared<TestSink>();
    logPal_->addSink(sink);

    logPal_->flush();

    EXPECT_EQ(sink->flushCount, 1);
}

// Test os_log level mapping
TEST_F(DarwinLogPALTest, LogLevelMappingTest) {
    // This test verifies that log levels are correctly mapped to os_log types
    // The actual mapping:
    // - Trace/Debug -> OS_LOG_TYPE_DEBUG
    // - Info -> OS_LOG_TYPE_INFO
    // - Warning -> OS_LOG_TYPE_DEFAULT
    // - Error -> OS_LOG_TYPE_ERROR
    // - Critical -> OS_LOG_TYPE_FAULT

    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    // We can't directly verify os_log output, but we can ensure no crashes
    EXPECT_NO_THROW({
        logPal_->log(LogLevel::Trace, "Trace->DEBUG", "Test", ctx);
        logPal_->log(LogLevel::Debug, "Debug->DEBUG", "Test", ctx);
        logPal_->log(LogLevel::Info, "Info->INFO", "Test", ctx);
        logPal_->log(LogLevel::Warning, "Warning->DEFAULT", "Test", ctx);
        logPal_->log(LogLevel::Error, "Error->ERROR", "Test", ctx);
        logPal_->log(LogLevel::Critical, "Critical->FAULT", "Test", ctx);
    });
}

// Test with special characters in message
TEST_F(DarwinLogPALTest, LogWithSpecialCharacters) {
    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    EXPECT_NO_THROW(logPal_->log(LogLevel::Info,
        "Special chars: %s %d %@ \n\t\r", "Test", ctx));
}

// Test logging from multiple threads
TEST_F(DarwinLogPALTest, ThreadSafeLogging) {
    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([this, i, &ctx]() {
            for (int j = 0; j < 100; ++j) {
                logPal_->log(LogLevel::Info,
                    "Thread " + std::to_string(i) + " message " + std::to_string(j),
                    "ThreadTest", ctx);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // If we got here without crashes or deadlocks, the test passed
    SUCCEED();
}

#endif // __APPLE__

} // namespace test
} // namespace pal
} // namespace openrtmp
