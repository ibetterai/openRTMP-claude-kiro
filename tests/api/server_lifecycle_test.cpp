// OpenRTMP - Cross-platform RTMP Server
// Server Lifecycle Integration Tests
//
// Tests for Task 20.2: Integrate all components into server lifecycle
// - Wire configuration loading during initialization
// - Start event loop and connection acceptance on server start
// - Coordinate component shutdown in correct order
// - Validate configuration and report errors on startup
// - Emit server lifecycle events to registered callbacks
//
// Requirements coverage:
// - Requirement 17.4: Validate configuration on startup
// - Requirement 17.5: Report errors for invalid settings

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <future>

#include "openrtmp/api/server_lifecycle.hpp"
#include "openrtmp/api/rtmp_server.hpp"
#include "openrtmp/core/config_manager.hpp"

using namespace openrtmp;
using namespace openrtmp::api;
using namespace openrtmp::core;

// =============================================================================
// Test Fixtures
// =============================================================================

class ServerLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        lifecycleManager_ = std::make_unique<ServerLifecycleManager>();
    }

    void TearDown() override {
        if (lifecycleManager_) {
            lifecycleManager_->forceShutdown();
        }
    }

    std::unique_ptr<ServerLifecycleManager> lifecycleManager_;
};

// =============================================================================
// Construction Tests
// =============================================================================

TEST_F(ServerLifecycleTest, DefaultConstruction) {
    EXPECT_NE(lifecycleManager_, nullptr);
    EXPECT_EQ(lifecycleManager_->state(), LifecycleState::Uninitialized);
}

TEST_F(ServerLifecycleTest, ConstructWithConfig) {
    LifecycleConfig config;
    config.port = 1935;
    config.bindAddress = "127.0.0.1";

    auto manager = std::make_unique<ServerLifecycleManager>(config);
    EXPECT_NE(manager, nullptr);
    EXPECT_EQ(manager->state(), LifecycleState::Uninitialized);
}

// =============================================================================
// Initialization Tests - Configuration Loading (Requirement 17.4)
// =============================================================================

TEST_F(ServerLifecycleTest, InitializeWithDefaultConfig) {
    auto result = lifecycleManager_->initialize();
    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(lifecycleManager_->state(), LifecycleState::Initialized);
}

TEST_F(ServerLifecycleTest, InitializeWithCustomConfig) {
    LifecycleConfig config;
    config.port = 1936;
    config.bindAddress = "127.0.0.1";
    config.maxConnections = 500;
    config.gracePeriodMs = 15000;

    auto result = lifecycleManager_->initialize(config);
    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(lifecycleManager_->state(), LifecycleState::Initialized);
}

TEST_F(ServerLifecycleTest, InitializeValidatesConfiguration) {
    LifecycleConfig config;
    config.port = 0;  // Invalid port

    auto result = lifecycleManager_->initialize(config);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, LifecycleError::Code::ConfigurationInvalid);
}

TEST_F(ServerLifecycleTest, InitializeWithInvalidTLSConfig) {
    LifecycleConfig config;
    config.port = 1935;
    config.enableTLS = true;
    // Missing certificate paths

    auto result = lifecycleManager_->initialize(config);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, LifecycleError::Code::ConfigurationInvalid);
}

TEST_F(ServerLifecycleTest, InitializeTwiceFails) {
    auto result1 = lifecycleManager_->initialize();
    EXPECT_TRUE(result1.isSuccess());

    auto result2 = lifecycleManager_->initialize();
    EXPECT_TRUE(result2.isError());
    EXPECT_EQ(result2.error().code, LifecycleError::Code::InvalidState);
}

TEST_F(ServerLifecycleTest, InitializeLoadsConfiguration) {
    LifecycleConfig config;
    config.port = 1937;
    config.maxConnections = 250;

    auto result = lifecycleManager_->initialize(config);
    EXPECT_TRUE(result.isSuccess());

    auto effectiveConfig = lifecycleManager_->getEffectiveConfig();
    EXPECT_EQ(effectiveConfig.port, 1937);
    EXPECT_EQ(effectiveConfig.maxConnections, 250);
}

// =============================================================================
// Startup Tests - Event Loop and Connection Acceptance
// =============================================================================

TEST_F(ServerLifecycleTest, StartAfterInitialize) {
    lifecycleManager_->initialize();

    auto result = lifecycleManager_->start();
    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(lifecycleManager_->state(), LifecycleState::Running);
}

TEST_F(ServerLifecycleTest, StartWithoutInitializeFails) {
    auto result = lifecycleManager_->start();
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, LifecycleError::Code::InvalidState);
}

TEST_F(ServerLifecycleTest, StartTwiceIsIdempotent) {
    lifecycleManager_->initialize();
    lifecycleManager_->start();

    auto result = lifecycleManager_->start();
    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(lifecycleManager_->state(), LifecycleState::Running);
}

TEST_F(ServerLifecycleTest, StartInitializesAllComponents) {
    lifecycleManager_->initialize();
    lifecycleManager_->start();

    EXPECT_TRUE(lifecycleManager_->isComponentInitialized("ConfigManager"));
    EXPECT_TRUE(lifecycleManager_->isComponentInitialized("ConnectionPool"));
    EXPECT_TRUE(lifecycleManager_->isComponentInitialized("StreamRegistry"));
    EXPECT_TRUE(lifecycleManager_->isComponentInitialized("AuthService"));
    EXPECT_TRUE(lifecycleManager_->isComponentInitialized("ShutdownCoordinator"));
    EXPECT_TRUE(lifecycleManager_->isComponentInitialized("ErrorIsolation"));
    EXPECT_TRUE(lifecycleManager_->isComponentInitialized("MetricsCollector"));
}

TEST_F(ServerLifecycleTest, StartWithTLSEnabled) {
    LifecycleConfig config;
    config.port = 1935;
    config.enableTLS = true;
    config.tlsCertPath = "/tmp/test_cert.pem";
    config.tlsKeyPath = "/tmp/test_key.pem";

    lifecycleManager_->initialize(config);

    // Start should work even if TLS certs don't exist (will log error but not fail)
    auto result = lifecycleManager_->start();
    // TLS failures are non-fatal in this implementation
    EXPECT_TRUE(result.isSuccess() || result.error().code == LifecycleError::Code::TLSInitializationFailed);
}

// =============================================================================
// Shutdown Tests - Correct Order
// =============================================================================

TEST_F(ServerLifecycleTest, GracefulShutdown) {
    LifecycleConfig config;
    config.gracePeriodMs = 10;  // Short grace period for faster tests
    lifecycleManager_->initialize(config);
    lifecycleManager_->start();

    auto result = lifecycleManager_->shutdown(true);
    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(lifecycleManager_->state(), LifecycleState::Stopped);
}

TEST_F(ServerLifecycleTest, ForceShutdown) {
    lifecycleManager_->initialize();
    lifecycleManager_->start();

    auto result = lifecycleManager_->forceShutdown();
    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(lifecycleManager_->state(), LifecycleState::Stopped);
}

TEST_F(ServerLifecycleTest, ShutdownOrder) {
    lifecycleManager_->initialize();
    lifecycleManager_->start();

    std::vector<std::string> shutdownOrder;
    lifecycleManager_->setComponentShutdownCallback([&shutdownOrder](const std::string& component) {
        shutdownOrder.push_back(component);
    });

    lifecycleManager_->shutdown(false);

    // Verify shutdown order: connections first, then services, then cleanup
    ASSERT_GE(shutdownOrder.size(), 3u);

    // Connection-related components should shut down before registry
    auto connPoolIdx = std::find(shutdownOrder.begin(), shutdownOrder.end(), "ConnectionPool");
    auto registryIdx = std::find(shutdownOrder.begin(), shutdownOrder.end(), "StreamRegistry");

    if (connPoolIdx != shutdownOrder.end() && registryIdx != shutdownOrder.end()) {
        EXPECT_LT(connPoolIdx, registryIdx);
    }
}

TEST_F(ServerLifecycleTest, ShutdownStopsAcceptingConnections) {
    LifecycleConfig config;
    config.gracePeriodMs = 10;  // Short grace period for faster tests
    lifecycleManager_->initialize(config);
    lifecycleManager_->start();

    EXPECT_TRUE(lifecycleManager_->canAcceptConnections());

    lifecycleManager_->shutdown(true);

    EXPECT_FALSE(lifecycleManager_->canAcceptConnections());
}

TEST_F(ServerLifecycleTest, ShutdownNotifiesPublishers) {
    LifecycleConfig config;
    config.gracePeriodMs = 10;  // Short grace period for faster tests
    lifecycleManager_->initialize(config);
    lifecycleManager_->start();

    std::atomic<bool> publisherNotified{false};
    lifecycleManager_->setPublisherNotificationCallback([&publisherNotified]() {
        publisherNotified = true;
    });

    lifecycleManager_->shutdown(true);

    EXPECT_TRUE(publisherNotified.load());
}

TEST_F(ServerLifecycleTest, ShutdownRespectsGracePeriod) {
    LifecycleConfig config;
    config.gracePeriodMs = 100;

    lifecycleManager_->initialize(config);
    lifecycleManager_->start();

    auto startTime = std::chrono::steady_clock::now();
    lifecycleManager_->shutdown(true);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime
    ).count();

    // Graceful shutdown should wait for grace period
    EXPECT_GE(elapsed, 90);  // Allow some margin
    EXPECT_LT(elapsed, 500);
}

TEST_F(ServerLifecycleTest, ForceShutdownIsImmediate) {
    lifecycleManager_->initialize();
    lifecycleManager_->start();

    auto startTime = std::chrono::steady_clock::now();
    lifecycleManager_->forceShutdown();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime
    ).count();

    EXPECT_LT(elapsed, 100);  // Should be nearly immediate
}

// =============================================================================
// Event Emission Tests
// =============================================================================

TEST_F(ServerLifecycleTest, EmitsServerInitializedEvent) {
    std::atomic<bool> eventReceived{false};
    LifecycleEventType receivedType = LifecycleEventType::ConfigurationError;

    lifecycleManager_->setEventCallback([&](const LifecycleEvent& event) {
        if (event.type == LifecycleEventType::ServerInitialized) {
            eventReceived = true;
            receivedType = event.type;
        }
    });

    lifecycleManager_->initialize();

    EXPECT_TRUE(eventReceived.load());
    EXPECT_EQ(receivedType, LifecycleEventType::ServerInitialized);
}

TEST_F(ServerLifecycleTest, EmitsServerStartedEvent) {
    std::atomic<bool> eventReceived{false};

    lifecycleManager_->initialize();
    lifecycleManager_->setEventCallback([&](const LifecycleEvent& event) {
        if (event.type == LifecycleEventType::ServerStarted) {
            eventReceived = true;
        }
    });

    lifecycleManager_->start();

    EXPECT_TRUE(eventReceived.load());
}

TEST_F(ServerLifecycleTest, EmitsServerStoppingEvent) {
    std::atomic<bool> eventReceived{false};

    lifecycleManager_->initialize();
    lifecycleManager_->start();

    lifecycleManager_->setEventCallback([&](const LifecycleEvent& event) {
        if (event.type == LifecycleEventType::ServerStopping) {
            eventReceived = true;
        }
    });

    lifecycleManager_->shutdown(false);

    EXPECT_TRUE(eventReceived.load());
}

TEST_F(ServerLifecycleTest, EmitsServerStoppedEvent) {
    std::atomic<bool> eventReceived{false};

    lifecycleManager_->initialize();
    lifecycleManager_->start();

    lifecycleManager_->setEventCallback([&](const LifecycleEvent& event) {
        if (event.type == LifecycleEventType::ServerStopped) {
            eventReceived = true;
        }
    });

    lifecycleManager_->shutdown(false);

    EXPECT_TRUE(eventReceived.load());
}

TEST_F(ServerLifecycleTest, EmitsConfigurationErrorEvent) {
    std::atomic<bool> eventReceived{false};
    std::string errorMessage;

    lifecycleManager_->setEventCallback([&](const LifecycleEvent& event) {
        if (event.type == LifecycleEventType::ConfigurationError) {
            eventReceived = true;
            errorMessage = event.message;
        }
    });

    LifecycleConfig config;
    config.port = 0;
    lifecycleManager_->initialize(config);

    EXPECT_TRUE(eventReceived.load());
    EXPECT_FALSE(errorMessage.empty());
}

TEST_F(ServerLifecycleTest, EmitsComponentErrorEvent) {
    std::atomic<bool> eventReceived{false};
    std::string componentName;

    lifecycleManager_->initialize();
    lifecycleManager_->start();

    lifecycleManager_->setEventCallback([&](const LifecycleEvent& event) {
        if (event.type == LifecycleEventType::ComponentError) {
            eventReceived = true;
            componentName = event.componentName;
        }
    });

    // Simulate a component error
    lifecycleManager_->reportComponentError("TestComponent", "Test error message");

    EXPECT_TRUE(eventReceived.load());
    EXPECT_EQ(componentName, "TestComponent");
}

// =============================================================================
// Configuration Validation Tests (Requirement 17.4, 17.5)
// =============================================================================

TEST_F(ServerLifecycleTest, ValidatesPortRange) {
    LifecycleConfig config;

    // Port 0 is invalid
    config.port = 0;
    auto result1 = lifecycleManager_->initialize(config);
    EXPECT_TRUE(result1.isError());

    // Valid port
    auto manager2 = std::make_unique<ServerLifecycleManager>();
    config.port = 1935;
    auto result2 = manager2->initialize(config);
    EXPECT_TRUE(result2.isSuccess());
}

TEST_F(ServerLifecycleTest, ValidatesMaxConnections) {
    LifecycleConfig config;
    config.port = 1935;
    config.maxConnections = 0;  // 0 should use platform defaults

    auto result = lifecycleManager_->initialize(config);
    EXPECT_TRUE(result.isSuccess());

    // Verify platform default was applied
    auto effective = lifecycleManager_->getEffectiveConfig();
    EXPECT_GT(effective.maxConnections, 0u);
}

TEST_F(ServerLifecycleTest, ValidatesTLSConfiguration) {
    LifecycleConfig config;
    config.port = 1935;
    config.enableTLS = true;
    config.tlsCertPath = "";  // Invalid - empty path
    config.tlsKeyPath = "";

    auto result = lifecycleManager_->initialize(config);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, LifecycleError::Code::ConfigurationInvalid);
}

TEST_F(ServerLifecycleTest, ReportsDetailedConfigurationErrors) {
    LifecycleConfig config;
    config.port = 0;

    auto result = lifecycleManager_->initialize(config);
    EXPECT_TRUE(result.isError());
    EXPECT_FALSE(result.error().message.empty());
    EXPECT_TRUE(result.error().message.find("port") != std::string::npos ||
                result.error().message.find("Port") != std::string::npos);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST_F(ServerLifecycleTest, ConcurrentStateAccess) {
    lifecycleManager_->initialize();
    lifecycleManager_->start();

    std::atomic<int> successCount{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < 10; i++) {
        threads.emplace_back([this, &successCount]() {
            for (int j = 0; j < 100; j++) {
                auto state = lifecycleManager_->state();
                if (state == LifecycleState::Running ||
                    state == LifecycleState::Stopping ||
                    state == LifecycleState::Stopped) {
                    successCount++;
                }
            }
        });
    }

    // Start shutdown in middle of accesses
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    lifecycleManager_->shutdown(false);

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_GE(successCount.load(), 100);  // At least some should succeed
}

TEST_F(ServerLifecycleTest, ConcurrentEventCallbacks) {
    std::atomic<int> eventCount{0};

    lifecycleManager_->setEventCallback([&eventCount](const LifecycleEvent&) {
        eventCount++;
    });

    lifecycleManager_->initialize();
    lifecycleManager_->start();

    // Multiple threads triggering events
    std::vector<std::thread> threads;
    for (int i = 0; i < 5; i++) {
        threads.emplace_back([this]() {
            for (int j = 0; j < 10; j++) {
                lifecycleManager_->reportComponentError("Test", "Error");
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    lifecycleManager_->shutdown(false);

    // All events should have been processed
    EXPECT_GE(eventCount.load(), 50);  // At least the error events
}

// =============================================================================
// Component Integration Tests
// =============================================================================

TEST_F(ServerLifecycleTest, IntegratesWithConfigManager) {
    lifecycleManager_->initialize();

    auto configManager = lifecycleManager_->getConfigManager();
    EXPECT_NE(configManager, nullptr);
}

TEST_F(ServerLifecycleTest, IntegratesWithConnectionPool) {
    lifecycleManager_->initialize();
    lifecycleManager_->start();

    auto connectionPool = lifecycleManager_->getConnectionPool();
    EXPECT_NE(connectionPool, nullptr);
}

TEST_F(ServerLifecycleTest, IntegratesWithStreamRegistry) {
    lifecycleManager_->initialize();
    lifecycleManager_->start();

    auto streamRegistry = lifecycleManager_->getStreamRegistry();
    EXPECT_NE(streamRegistry, nullptr);
}

TEST_F(ServerLifecycleTest, IntegratesWithMetricsCollector) {
    lifecycleManager_->initialize();
    lifecycleManager_->start();

    auto metricsCollector = lifecycleManager_->getMetricsCollector();
    EXPECT_NE(metricsCollector, nullptr);
}

TEST_F(ServerLifecycleTest, IntegratesWithShutdownCoordinator) {
    lifecycleManager_->initialize();
    lifecycleManager_->start();

    auto shutdownCoordinator = lifecycleManager_->getShutdownCoordinator();
    EXPECT_NE(shutdownCoordinator, nullptr);
}

TEST_F(ServerLifecycleTest, IntegratesWithErrorIsolation) {
    lifecycleManager_->initialize();
    lifecycleManager_->start();

    auto errorIsolation = lifecycleManager_->getErrorIsolation();
    EXPECT_NE(errorIsolation, nullptr);
}

// =============================================================================
// Restart Tests
// =============================================================================

TEST_F(ServerLifecycleTest, RestartAfterShutdown) {
    lifecycleManager_->initialize();
    lifecycleManager_->start();
    EXPECT_EQ(lifecycleManager_->state(), LifecycleState::Running);

    lifecycleManager_->shutdown(false);
    EXPECT_EQ(lifecycleManager_->state(), LifecycleState::Stopped);

    // Should be able to restart
    auto result = lifecycleManager_->start();
    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(lifecycleManager_->state(), LifecycleState::Running);
}

TEST_F(ServerLifecycleTest, RestartPreservesEventCallbacks) {
    std::atomic<int> startedCount{0};

    lifecycleManager_->setEventCallback([&startedCount](const LifecycleEvent& event) {
        if (event.type == LifecycleEventType::ServerStarted) {
            startedCount++;
        }
    });

    lifecycleManager_->initialize();
    lifecycleManager_->start();
    lifecycleManager_->shutdown(false);
    lifecycleManager_->start();
    lifecycleManager_->shutdown(false);

    EXPECT_EQ(startedCount.load(), 2);
}

// =============================================================================
// Health Check Tests
// =============================================================================

TEST_F(ServerLifecycleTest, GetHealthStatus) {
    lifecycleManager_->initialize();
    lifecycleManager_->start();

    auto health = lifecycleManager_->getHealthStatus();
    EXPECT_TRUE(health.healthy);
}

TEST_F(ServerLifecycleTest, GetHealthStatusBeforeStart) {
    lifecycleManager_->initialize();

    auto health = lifecycleManager_->getHealthStatus();
    EXPECT_FALSE(health.healthy);
    EXPECT_FALSE(health.details.empty());
}

TEST_F(ServerLifecycleTest, GetHealthStatusAfterShutdown) {
    lifecycleManager_->initialize();
    lifecycleManager_->start();
    lifecycleManager_->shutdown(false);

    auto health = lifecycleManager_->getHealthStatus();
    EXPECT_FALSE(health.healthy);
}
