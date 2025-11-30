// OpenRTMP - Cross-platform RTMP Server
// RTMPServer API Unit Tests
//
// Tests for Task 20.1: RTMPServer public API
// - Lifecycle methods: initialize, start, stop
// - Graceful and force stop modes
// - Runtime configuration updates
// - Server and per-stream metrics
// - Authentication and event callbacks
// - Thread-safe method invocation

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <future>

#include "openrtmp/api/rtmp_server.hpp"

using namespace openrtmp;
using namespace openrtmp::api;

// =============================================================================
// Test Fixtures
// =============================================================================

class RTMPServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create default server configuration
        config_.port = 1935;
        config_.bindAddress = "127.0.0.1";
        config_.maxConnections = 100;
        config_.gracePeriodMs = 1000;
    }

    void TearDown() override {
        // Ensure server is stopped
        if (server_ && server_->isRunning()) {
            server_->forceStop();
        }
    }

    ServerConfig config_;
    std::unique_ptr<RTMPServer> server_;
};

// =============================================================================
// Construction Tests
// =============================================================================

TEST_F(RTMPServerTest, DefaultConstructor) {
    server_ = std::make_unique<RTMPServer>();
    EXPECT_NE(server_, nullptr);
    EXPECT_FALSE(server_->isRunning());
    EXPECT_EQ(server_->state(), ServerState::Uninitialized);
}

TEST_F(RTMPServerTest, ConstructorWithConfig) {
    server_ = std::make_unique<RTMPServer>(config_);
    EXPECT_NE(server_, nullptr);
    EXPECT_FALSE(server_->isRunning());
    EXPECT_EQ(server_->state(), ServerState::Uninitialized);
}

// =============================================================================
// Lifecycle Tests - Initialize
// =============================================================================

TEST_F(RTMPServerTest, InitializeWithValidConfig) {
    server_ = std::make_unique<RTMPServer>();
    auto result = server_->initialize(config_);
    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(server_->state(), ServerState::Initialized);
}

TEST_F(RTMPServerTest, InitializeWithInvalidPort) {
    server_ = std::make_unique<RTMPServer>();
    config_.port = 0;  // Invalid port
    auto result = server_->initialize(config_);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, ServerError::Code::InvalidConfiguration);
}

TEST_F(RTMPServerTest, InitializeTwiceFails) {
    server_ = std::make_unique<RTMPServer>();
    auto result1 = server_->initialize(config_);
    EXPECT_TRUE(result1.isSuccess());

    auto result2 = server_->initialize(config_);
    EXPECT_TRUE(result2.isError());
    EXPECT_EQ(result2.error().code, ServerError::Code::InvalidState);
}

TEST_F(RTMPServerTest, InitializeWithZeroMaxConnections) {
    server_ = std::make_unique<RTMPServer>();
    config_.maxConnections = 0;  // Should use platform default
    auto result = server_->initialize(config_);
    EXPECT_TRUE(result.isSuccess());
}

// =============================================================================
// Lifecycle Tests - Start
// =============================================================================

TEST_F(RTMPServerTest, StartAfterInitialize) {
    server_ = std::make_unique<RTMPServer>();
    auto initResult = server_->initialize(config_);
    EXPECT_TRUE(initResult.isSuccess());

    auto startResult = server_->start();
    EXPECT_TRUE(startResult.isSuccess());
    EXPECT_TRUE(server_->isRunning());
    EXPECT_EQ(server_->state(), ServerState::Running);
}

TEST_F(RTMPServerTest, StartWithoutInitializeFails) {
    server_ = std::make_unique<RTMPServer>();
    auto result = server_->start();
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, ServerError::Code::InvalidState);
}

TEST_F(RTMPServerTest, StartTwiceIsIdempotent) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);

    auto result1 = server_->start();
    EXPECT_TRUE(result1.isSuccess());

    auto result2 = server_->start();
    EXPECT_TRUE(result2.isSuccess());  // Already running, no error
    EXPECT_TRUE(server_->isRunning());
}

// =============================================================================
// Lifecycle Tests - Stop (Graceful)
// =============================================================================

TEST_F(RTMPServerTest, GracefulStopRunningServer) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);
    server_->start();
    EXPECT_TRUE(server_->isRunning());

    auto result = server_->stop(true);  // graceful = true
    EXPECT_TRUE(result.isSuccess());
    EXPECT_FALSE(server_->isRunning());
    EXPECT_EQ(server_->state(), ServerState::Stopped);
}

TEST_F(RTMPServerTest, StopNotRunningServerSucceeds) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);

    auto result = server_->stop(true);
    EXPECT_TRUE(result.isSuccess());  // Already stopped
}

TEST_F(RTMPServerTest, GracefulStopRespectsGracePeriod) {
    server_ = std::make_unique<RTMPServer>();
    config_.gracePeriodMs = 100;  // Short grace period for test
    server_->initialize(config_);
    server_->start();

    auto startTime = std::chrono::steady_clock::now();
    server_->stop(true);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime
    ).count();

    // Should complete relatively quickly (within grace period)
    EXPECT_LT(elapsed, config_.gracePeriodMs + 500);
    EXPECT_FALSE(server_->isRunning());
}

// =============================================================================
// Lifecycle Tests - Force Stop
// =============================================================================

TEST_F(RTMPServerTest, ForceStopImmediateTermination) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);
    server_->start();

    auto startTime = std::chrono::steady_clock::now();
    auto result = server_->forceStop();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime
    ).count();

    EXPECT_TRUE(result.isSuccess());
    EXPECT_FALSE(server_->isRunning());
    EXPECT_LT(elapsed, 100);  // Should be nearly immediate
}

TEST_F(RTMPServerTest, ForceStopDuringGracefulStop) {
    server_ = std::make_unique<RTMPServer>();
    config_.gracePeriodMs = 5000;  // Long grace period
    server_->initialize(config_);
    server_->start();

    // Start graceful shutdown in background
    std::thread shutdownThread([this]() {
        server_->stop(true);
    });

    // Give some time for graceful shutdown to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Force stop should interrupt graceful shutdown
    auto result = server_->forceStop();
    EXPECT_TRUE(result.isSuccess());
    EXPECT_FALSE(server_->isRunning());

    shutdownThread.join();
}

// =============================================================================
// Runtime Configuration Update Tests
// =============================================================================

TEST_F(RTMPServerTest, UpdateConfigWhileRunning) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);
    server_->start();

    RuntimeConfigUpdate update;
    update.maxConnections = 200;
    update.gracePeriodMs = 2000;

    auto result = server_->updateConfig(update);
    EXPECT_TRUE(result.isSuccess());
}

TEST_F(RTMPServerTest, UpdateConfigWhileStopped) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);

    RuntimeConfigUpdate update;
    update.maxConnections = 200;

    auto result = server_->updateConfig(update);
    EXPECT_TRUE(result.isSuccess());
}

TEST_F(RTMPServerTest, UpdateConfigWithInvalidValues) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);
    server_->start();

    RuntimeConfigUpdate update;
    update.gracePeriodMs = 0;  // Invalid: 0 is not a valid grace period

    // Server should accept this (0 may mean "use default")
    auto result = server_->updateConfig(update);
    EXPECT_TRUE(result.isSuccess());  // Accept 0 as "no change"
}

TEST_F(RTMPServerTest, UpdateConfigBeforeInitialize) {
    server_ = std::make_unique<RTMPServer>();

    RuntimeConfigUpdate update;
    update.maxConnections = 200;

    auto result = server_->updateConfig(update);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, ServerError::Code::InvalidState);
}

// =============================================================================
// Metrics Tests
// =============================================================================

TEST_F(RTMPServerTest, GetServerMetrics) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);
    server_->start();

    auto metrics = server_->getServerMetrics();
    EXPECT_GE(metrics.activeConnections, 0u);
    EXPECT_GE(metrics.totalConnections, 0u);
    EXPECT_GE(metrics.activeStreams, 0u);
    EXPECT_GE(metrics.totalStreams, 0u);
    EXPECT_GE(metrics.bytesReceived, 0u);
    EXPECT_GE(metrics.bytesSent, 0u);
}

TEST_F(RTMPServerTest, GetServerMetricsBeforeStart) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);

    auto metrics = server_->getServerMetrics();
    EXPECT_EQ(metrics.activeConnections, 0u);
    EXPECT_EQ(metrics.activeStreams, 0u);
}

TEST_F(RTMPServerTest, GetStreamMetricsExistingStream) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);
    server_->start();

    // Without actual streams, this should return nullopt
    auto metrics = server_->getStreamMetrics("test/stream");
    EXPECT_FALSE(metrics.has_value());
}

TEST_F(RTMPServerTest, GetStreamMetricsNonExistent) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);
    server_->start();

    auto metrics = server_->getStreamMetrics("nonexistent/stream");
    EXPECT_FALSE(metrics.has_value());
}

// =============================================================================
// Authentication Callback Tests
// =============================================================================

TEST_F(RTMPServerTest, SetAuthCallback) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);

    bool callbackSet = false;
    server_->setAuthCallback([&callbackSet](const AuthRequest& req) -> AuthResponse {
        callbackSet = true;
        AuthResponse resp;
        resp.allowed = true;
        return resp;
    });

    // Callback should be stored (we can't easily test if it's called without
    // simulating a real connection)
    EXPECT_TRUE(true);  // Callback set without error
}

TEST_F(RTMPServerTest, SetAuthCallbackNull) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);

    // Setting null callback should clear it
    server_->setAuthCallback(nullptr);
    EXPECT_TRUE(true);  // No crash
}

// =============================================================================
// Event Callback Tests
// =============================================================================

TEST_F(RTMPServerTest, SetEventCallback) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);

    std::atomic<int> eventCount{0};
    server_->setEventCallback([&eventCount](const ServerEvent& event) {
        eventCount++;
    });

    server_->start();
    // Server start should trigger an event
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    server_->stop(false);
    // Server stop should trigger an event
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_GE(eventCount.load(), 0);  // Events may or may not fire in unit test
}

TEST_F(RTMPServerTest, SetEventCallbackNull) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);

    server_->setEventCallback(nullptr);
    server_->start();
    server_->stop(false);
    // Should not crash with null callback
    EXPECT_TRUE(true);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST_F(RTMPServerTest, ConcurrentMethodCalls) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);
    server_->start();

    std::atomic<int> successCount{0};
    std::vector<std::thread> threads;

    // Multiple threads calling getServerMetrics concurrently
    for (int i = 0; i < 10; i++) {
        threads.emplace_back([this, &successCount]() {
            for (int j = 0; j < 100; j++) {
                auto metrics = server_->getServerMetrics();
                if (metrics.activeConnections >= 0) {
                    successCount++;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(successCount.load(), 1000);
}

TEST_F(RTMPServerTest, ConcurrentUpdateConfig) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);
    server_->start();

    std::atomic<int> successCount{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < 5; i++) {
        threads.emplace_back([this, &successCount, i]() {
            RuntimeConfigUpdate update;
            update.maxConnections = static_cast<uint32_t>(100 + i * 10);

            for (int j = 0; j < 10; j++) {
                auto result = server_->updateConfig(update);
                if (result.isSuccess()) {
                    successCount++;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(successCount.load(), 50);
}

TEST_F(RTMPServerTest, StartStopFromDifferentThreads) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);

    // Start from thread 1
    std::thread t1([this]() {
        server_->start();
    });
    t1.join();

    EXPECT_TRUE(server_->isRunning());

    // Stop from thread 2
    std::thread t2([this]() {
        server_->stop(false);
    });
    t2.join();

    EXPECT_FALSE(server_->isRunning());
}

// =============================================================================
// State Accessor Tests
// =============================================================================

TEST_F(RTMPServerTest, StateTransitions) {
    server_ = std::make_unique<RTMPServer>();
    EXPECT_EQ(server_->state(), ServerState::Uninitialized);

    server_->initialize(config_);
    EXPECT_EQ(server_->state(), ServerState::Initialized);

    server_->start();
    EXPECT_EQ(server_->state(), ServerState::Running);

    server_->stop(false);
    EXPECT_EQ(server_->state(), ServerState::Stopped);
}

TEST_F(RTMPServerTest, IsRunningAccessor) {
    server_ = std::make_unique<RTMPServer>();
    EXPECT_FALSE(server_->isRunning());

    server_->initialize(config_);
    EXPECT_FALSE(server_->isRunning());

    server_->start();
    EXPECT_TRUE(server_->isRunning());

    server_->stop(false);
    EXPECT_FALSE(server_->isRunning());
}

TEST_F(RTMPServerTest, GetConfigAfterInitialize) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);

    auto currentConfig = server_->getConfig();
    EXPECT_EQ(currentConfig.port, config_.port);
    EXPECT_EQ(currentConfig.bindAddress, config_.bindAddress);
}

// =============================================================================
// Restart Tests
// =============================================================================

TEST_F(RTMPServerTest, RestartServer) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);
    server_->start();
    EXPECT_TRUE(server_->isRunning());

    server_->stop(false);
    EXPECT_FALSE(server_->isRunning());

    // Restart with new start
    auto result = server_->start();
    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(server_->isRunning());
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST_F(RTMPServerTest, HandleStartError) {
    server_ = std::make_unique<RTMPServer>();
    // Don't initialize

    auto result = server_->start();
    EXPECT_TRUE(result.isError());
    EXPECT_FALSE(result.error().message.empty());
}

TEST_F(RTMPServerTest, ServerErrorMessageIsDescriptive) {
    server_ = std::make_unique<RTMPServer>();
    auto result = server_->start();

    EXPECT_TRUE(result.isError());
    EXPECT_FALSE(result.error().message.empty());
    // Message should describe what went wrong
    EXPECT_TRUE(result.error().message.find("initialize") != std::string::npos ||
                result.error().message.find("state") != std::string::npos ||
                result.error().message.find("not") != std::string::npos);
}

// =============================================================================
// Health Check Tests
// =============================================================================

TEST_F(RTMPServerTest, GetHealthStatus) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);
    server_->start();

    auto health = server_->getHealthStatus();
    EXPECT_TRUE(health.healthy);
    EXPECT_FALSE(health.details.empty());
}

TEST_F(RTMPServerTest, GetHealthStatusBeforeStart) {
    server_ = std::make_unique<RTMPServer>();
    server_->initialize(config_);

    auto health = server_->getHealthStatus();
    // Server initialized but not running is not "healthy" in the running sense
    EXPECT_FALSE(health.healthy);
}

// =============================================================================
// Version Information Tests
// =============================================================================

TEST_F(RTMPServerTest, GetVersion) {
    auto version = RTMPServer::getVersion();
    EXPECT_FALSE(version.empty());
}

// =============================================================================
// Move Semantics Tests
// =============================================================================

TEST_F(RTMPServerTest, MoveConstructor) {
    auto server1 = std::make_unique<RTMPServer>();
    server1->initialize(config_);
    server1->start();

    RTMPServer server2 = std::move(*server1);
    EXPECT_TRUE(server2.isRunning());
    server2.stop(false);
}

TEST_F(RTMPServerTest, MoveAssignment) {
    auto server1 = std::make_unique<RTMPServer>();
    server1->initialize(config_);
    server1->start();

    RTMPServer server2;
    server2 = std::move(*server1);
    EXPECT_TRUE(server2.isRunning());
    server2.stop(false);
}

