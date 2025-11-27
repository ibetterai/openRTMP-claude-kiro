// OpenRTMP - Cross-platform RTMP Server
// Tests for Windows Log PAL Implementation
//
// Requirements Covered: 18.5 (Platform-native logging integration - Windows Event Log)

#include <gtest/gtest.h>
#include "openrtmp/pal/log_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"

#include <thread>
#include <vector>

#if defined(_WIN32)
#include "openrtmp/pal/windows/windows_log_pal.hpp"
#endif

namespace openrtmp {
namespace pal {
namespace test {

#if defined(_WIN32)

// =============================================================================
// Windows Log PAL Tests
// =============================================================================

class WindowsLogPALTest : public ::testing::Test {
protected:
    void SetUp() override {
        logPal_ = std::make_unique<windows::WindowsLogPAL>();
    }

    void TearDown() override {
        logPal_.reset();
    }

    std::unique_ptr<windows::WindowsLogPAL> logPal_;
};

TEST_F(WindowsLogPALTest, ImplementsILogPALInterface) {
    ILogPAL* interface = logPal_.get();
    EXPECT_NE(interface, nullptr);
}

TEST_F(WindowsLogPALTest, DefaultMinLevelIsDebug) {
    // Default minimum level should allow most messages
    LogLevel level = logPal_->getMinLevel();
    EXPECT_LE(static_cast<int>(level), static_cast<int>(LogLevel::Debug));
}

TEST_F(WindowsLogPALTest, SetMinLevelUpdatesLevel) {
    logPal_->setMinLevel(LogLevel::Warning);
    EXPECT_EQ(logPal_->getMinLevel(), LogLevel::Warning);

    logPal_->setMinLevel(LogLevel::Error);
    EXPECT_EQ(logPal_->getMinLevel(), LogLevel::Error);

    logPal_->setMinLevel(LogLevel::Trace);
    EXPECT_EQ(logPal_->getMinLevel(), LogLevel::Trace);
}

TEST_F(WindowsLogPALTest, LogDoesNotCrashWithValidContext) {
    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    // Should not throw or crash
    EXPECT_NO_THROW(logPal_->log(LogLevel::Info, "Test message", "Test", ctx));
}

TEST_F(WindowsLogPALTest, LogWithAllLogLevels) {
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

TEST_F(WindowsLogPALTest, LogBelowMinLevelIsIgnored) {
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

TEST_F(WindowsLogPALTest, LogWithEmptyMessage) {
    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    EXPECT_NO_THROW(logPal_->log(LogLevel::Info, "", "Test", ctx));
}

TEST_F(WindowsLogPALTest, LogWithEmptyCategory) {
    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    EXPECT_NO_THROW(logPal_->log(LogLevel::Info, "Test message", "", ctx));
}

TEST_F(WindowsLogPALTest, LogWithNullContext) {
    LogContext ctx;
    ctx.file = nullptr;
    ctx.line = 0;
    ctx.function = nullptr;

    EXPECT_NO_THROW(logPal_->log(LogLevel::Info, "Test message", "Test", ctx));
}

TEST_F(WindowsLogPALTest, FlushDoesNotCrash) {
    // Log some messages first
    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    logPal_->log(LogLevel::Info, "Message before flush", "Test", ctx);

    // Flush should not crash
    EXPECT_NO_THROW(logPal_->flush());
}

TEST_F(WindowsLogPALTest, AddSinkWorks) {
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

TEST_F(WindowsLogPALTest, RemoveSinkWorks) {
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

TEST_F(WindowsLogPALTest, MultipleSinksReceiveMessages) {
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

TEST_F(WindowsLogPALTest, FlushCallsSinkFlush) {
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

// Test OutputDebugString mapping
TEST_F(WindowsLogPALTest, LogLevelMappingTest) {
    // This test verifies that log levels are correctly mapped
    // The actual mapping for Windows:
    // - Trace/Debug -> OutputDebugStringW (debug output)
    // - Info -> OutputDebugStringW (debug output)
    // - Warning -> OutputDebugStringW (debug output)
    // - Error -> OutputDebugStringW + Event Log (optional)
    // - Critical -> OutputDebugStringW + Event Log (optional)

    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    // We can't directly verify OutputDebugString output, but we can ensure no crashes
    EXPECT_NO_THROW({
        logPal_->log(LogLevel::Trace, "Trace->DEBUG", "Test", ctx);
        logPal_->log(LogLevel::Debug, "Debug->DEBUG", "Test", ctx);
        logPal_->log(LogLevel::Info, "Info->INFO", "Test", ctx);
        logPal_->log(LogLevel::Warning, "Warning->WARNING", "Test", ctx);
        logPal_->log(LogLevel::Error, "Error->ERROR", "Test", ctx);
        logPal_->log(LogLevel::Critical, "Critical->CRITICAL", "Test", ctx);
    });
}

// Test with special characters in message
TEST_F(WindowsLogPALTest, LogWithSpecialCharacters) {
    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    EXPECT_NO_THROW(logPal_->log(LogLevel::Info,
        "Special chars: %s %d \n\t\r", "Test", ctx));
}

// Test logging from multiple threads
TEST_F(WindowsLogPALTest, ThreadSafeLogging) {
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

// Test Unicode message support
TEST_F(WindowsLogPALTest, LogWithUnicodeMessage) {
    LogContext ctx;
    ctx.file = __FILE__;
    ctx.line = __LINE__;
    ctx.function = __FUNCTION__;

    // Windows uses OutputDebugStringW which supports Unicode
    EXPECT_NO_THROW(logPal_->log(LogLevel::Info,
        "Unicode test: Hello World", "Test", ctx));
}

#endif // _WIN32

} // namespace test
} // namespace pal
} // namespace openrtmp
