// OpenRTMP - Cross-platform RTMP Server
// Error Isolation and Recovery Tests
//
// TDD test file for error isolation and recovery implementation.
// Tests written before implementation per TDD methodology.
//
// Requirements coverage:
// - Requirement 20.1: Single connection failure isolation
// - Requirement 20.2: Memory allocation failure handling
// - Requirement 20.3: Thread/task crash recovery
// - Requirement 20.4: Circuit breaker for external service failures
// - Requirement 20.5: Diagnostic logging before unrecoverable error termination
// - Requirement 20.6: Health check API for external monitoring

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <functional>
#include <memory>

#include "openrtmp/core/error_isolation.hpp"

namespace openrtmp {
namespace core {
namespace {

using namespace std::chrono_literals;

// =============================================================================
// Test Fixtures
// =============================================================================

class ErrorIsolationTest : public ::testing::Test {
protected:
    void SetUp() override {
        ErrorIsolationConfig config;
        config.memoryThresholdPercent = 90;
        config.componentRestartMaxAttempts = 3;
        config.componentRestartDelayMs = 100;
        config.circuitBreakerThreshold = 5;
        config.circuitBreakerResetMs = 1000;
        errorIsolation_ = std::make_unique<ErrorIsolation>(config);
    }

    void TearDown() override {
        errorIsolation_.reset();
    }

    std::unique_ptr<ErrorIsolation> errorIsolation_;
};

// =============================================================================
// Connection Error Isolation Tests (Requirement 20.1)
// =============================================================================

TEST_F(ErrorIsolationTest, ConnectionErrorDoesNotAffectOtherConnections) {
    // Simulate multiple connections
    ConnectionId conn1 = 1;
    ConnectionId conn2 = 2;
    ConnectionId conn3 = 3;

    errorIsolation_->registerConnection(conn1);
    errorIsolation_->registerConnection(conn2);
    errorIsolation_->registerConnection(conn3);

    EXPECT_EQ(3u, errorIsolation_->connectionCount());

    // Simulate error on conn1
    errorIsolation_->handleConnectionError(conn1, ErrorCode::ConnectionReset, "Connection reset by peer");

    // conn1 should be marked as failed, others should be active
    EXPECT_TRUE(errorIsolation_->isConnectionFailed(conn1));
    EXPECT_FALSE(errorIsolation_->isConnectionFailed(conn2));
    EXPECT_FALSE(errorIsolation_->isConnectionFailed(conn3));

    // Active connection count should not change until cleanup
    EXPECT_EQ(2u, errorIsolation_->activeConnectionCount());
}

TEST_F(ErrorIsolationTest, ConnectionErrorCallbackInvoked) {
    ConnectionId failedConn = 0;
    ErrorCode failedError = ErrorCode::Success;
    std::string failedMessage;

    errorIsolation_->setConnectionErrorCallback(
        [&](ConnectionId connId, ErrorCode code, const std::string& msg) {
            failedConn = connId;
            failedError = code;
            failedMessage = msg;
        }
    );

    ConnectionId conn1 = 1;
    errorIsolation_->registerConnection(conn1);
    errorIsolation_->handleConnectionError(conn1, ErrorCode::SendFailed, "Send buffer overflow");

    EXPECT_EQ(conn1, failedConn);
    EXPECT_EQ(ErrorCode::SendFailed, failedError);
    EXPECT_EQ("Send buffer overflow", failedMessage);
}

TEST_F(ErrorIsolationTest, ConnectionErrorStatisticsTracked) {
    ConnectionId conn1 = 1;
    ConnectionId conn2 = 2;

    errorIsolation_->registerConnection(conn1);
    errorIsolation_->registerConnection(conn2);

    errorIsolation_->handleConnectionError(conn1, ErrorCode::ConnectionReset, "Reset");
    errorIsolation_->handleConnectionError(conn2, ErrorCode::Timeout, "Timeout");

    auto stats = errorIsolation_->getStatistics();
    EXPECT_EQ(2u, stats.connectionFailures);
}

TEST_F(ErrorIsolationTest, CleanupFailedConnection) {
    ConnectionId conn1 = 1;
    errorIsolation_->registerConnection(conn1);
    errorIsolation_->handleConnectionError(conn1, ErrorCode::ConnectionClosed, "Closed");

    EXPECT_TRUE(errorIsolation_->isConnectionFailed(conn1));

    // Cleanup should remove the connection
    errorIsolation_->cleanupConnection(conn1);
    EXPECT_EQ(0u, errorIsolation_->connectionCount());
}

// =============================================================================
// Memory Pressure Detection Tests (Requirement 20.2)
// =============================================================================

TEST_F(ErrorIsolationTest, MemoryPressureDetectedAtThreshold) {
    // Set memory threshold to 90%
    errorIsolation_->updateConfig([](ErrorIsolationConfig& config) {
        config.memoryThresholdPercent = 90;
    });

    // Simulate low memory condition
    errorIsolation_->reportMemoryUsage(85, 100);  // 85% - below threshold
    EXPECT_FALSE(errorIsolation_->isUnderMemoryPressure());
    EXPECT_TRUE(errorIsolation_->canAcceptNewConnection());

    errorIsolation_->reportMemoryUsage(92, 100);  // 92% - above threshold
    EXPECT_TRUE(errorIsolation_->isUnderMemoryPressure());
    EXPECT_FALSE(errorIsolation_->canAcceptNewConnection());
}

TEST_F(ErrorIsolationTest, MemoryAllocationFailureTracked) {
    errorIsolation_->recordMemoryAllocationFailure("GOPBuffer", 1024 * 1024);

    auto stats = errorIsolation_->getStatistics();
    EXPECT_EQ(1u, stats.memoryAllocationFailures);
}

TEST_F(ErrorIsolationTest, MemoryPressureCallbackInvoked) {
    std::atomic<bool> pressureCallbackInvoked{false};
    uint64_t reportedUsedBytes = 0;
    uint64_t reportedTotalBytes = 0;

    errorIsolation_->setMemoryPressureCallback(
        [&](uint64_t usedBytes, uint64_t totalBytes) {
            pressureCallbackInvoked = true;
            reportedUsedBytes = usedBytes;
            reportedTotalBytes = totalBytes;
        }
    );

    errorIsolation_->reportMemoryUsage(95, 100);  // Above default 90% threshold

    EXPECT_TRUE(pressureCallbackInvoked);
    EXPECT_EQ(95u, reportedUsedBytes);
    EXPECT_EQ(100u, reportedTotalBytes);
}

TEST_F(ErrorIsolationTest, MemoryRecoveryRestoresAcceptance) {
    errorIsolation_->reportMemoryUsage(95, 100);  // Above threshold
    EXPECT_FALSE(errorIsolation_->canAcceptNewConnection());

    errorIsolation_->reportMemoryUsage(80, 100);  // Below threshold
    EXPECT_TRUE(errorIsolation_->canAcceptNewConnection());
}

// =============================================================================
// Component Restart Tests (Requirement 20.3)
// =============================================================================

TEST_F(ErrorIsolationTest, ComponentRestartAttemptedOnCrash) {
    std::atomic<int> restartCount{0};
    std::string restartedComponent;

    errorIsolation_->registerComponent("MediaHandler",
        [&]() -> bool {
            restartCount++;
            restartedComponent = "MediaHandler";
            return true;  // Restart successful
        }
    );

    errorIsolation_->reportComponentCrash("MediaHandler", "Segmentation fault");

    // Wait for restart attempt
    std::this_thread::sleep_for(200ms);

    EXPECT_EQ(1, restartCount);
    EXPECT_EQ("MediaHandler", restartedComponent);
}

TEST_F(ErrorIsolationTest, ComponentRestartMaxAttemptsRespected) {
    std::atomic<int> restartAttempts{0};

    errorIsolation_->updateConfig([](ErrorIsolationConfig& config) {
        config.componentRestartMaxAttempts = 3;
        config.componentRestartDelayMs = 10;
    });

    errorIsolation_->registerComponent("FaultyComponent",
        [&]() -> bool {
            restartAttempts++;
            return false;  // Restart always fails
        }
    );

    errorIsolation_->reportComponentCrash("FaultyComponent", "Critical error");

    // Wait for all restart attempts
    std::this_thread::sleep_for(100ms);

    EXPECT_EQ(3, restartAttempts);  // Should not exceed max attempts
}

TEST_F(ErrorIsolationTest, ComponentCrashStatisticsTracked) {
    errorIsolation_->registerComponent("TestComponent", []() { return true; });
    errorIsolation_->reportComponentCrash("TestComponent", "Test crash");

    auto stats = errorIsolation_->getStatistics();
    EXPECT_EQ(1u, stats.componentCrashes);
}

TEST_F(ErrorIsolationTest, ComponentStateTracked) {
    errorIsolation_->registerComponent("TestComponent", []() { return true; });

    EXPECT_EQ(ComponentState::Running, errorIsolation_->getComponentState("TestComponent"));

    errorIsolation_->reportComponentCrash("TestComponent", "Crash");
    std::this_thread::sleep_for(50ms);

    // After crash but before restart, should be Restarting
    // After successful restart, should be Running
    auto state = errorIsolation_->getComponentState("TestComponent");
    EXPECT_TRUE(state == ComponentState::Restarting || state == ComponentState::Running);
}

TEST_F(ErrorIsolationTest, UnrecoverableComponentMarkedAsFailed) {
    errorIsolation_->updateConfig([](ErrorIsolationConfig& config) {
        config.componentRestartMaxAttempts = 1;
        config.componentRestartDelayMs = 10;
    });

    errorIsolation_->registerComponent("CriticalComponent",
        []() { return false; }  // Always fails
    );

    errorIsolation_->reportComponentCrash("CriticalComponent", "Fatal error");

    std::this_thread::sleep_for(50ms);

    EXPECT_EQ(ComponentState::Failed, errorIsolation_->getComponentState("CriticalComponent"));
}

// =============================================================================
// Circuit Breaker Tests (Requirement 20.4)
// =============================================================================

TEST_F(ErrorIsolationTest, CircuitBreakerInitiallyClosed) {
    errorIsolation_->registerExternalService("AuthService");

    EXPECT_EQ(CircuitBreakerState::Closed,
              errorIsolation_->getCircuitBreakerState("AuthService"));
}

TEST_F(ErrorIsolationTest, CircuitBreakerOpensAfterThreshold) {
    errorIsolation_->updateConfig([](ErrorIsolationConfig& config) {
        config.circuitBreakerThreshold = 3;
    });

    errorIsolation_->registerExternalService("AuthService");

    // Record failures up to threshold
    errorIsolation_->recordExternalServiceFailure("AuthService");
    errorIsolation_->recordExternalServiceFailure("AuthService");
    EXPECT_EQ(CircuitBreakerState::Closed,
              errorIsolation_->getCircuitBreakerState("AuthService"));

    errorIsolation_->recordExternalServiceFailure("AuthService");
    EXPECT_EQ(CircuitBreakerState::Open,
              errorIsolation_->getCircuitBreakerState("AuthService"));
}

TEST_F(ErrorIsolationTest, CircuitBreakerRejectsWhenOpen) {
    errorIsolation_->updateConfig([](ErrorIsolationConfig& config) {
        config.circuitBreakerThreshold = 1;
    });

    errorIsolation_->registerExternalService("AuthService");
    errorIsolation_->recordExternalServiceFailure("AuthService");

    EXPECT_FALSE(errorIsolation_->isExternalServiceAvailable("AuthService"));
}

TEST_F(ErrorIsolationTest, CircuitBreakerTransitionsToHalfOpen) {
    errorIsolation_->updateConfig([](ErrorIsolationConfig& config) {
        config.circuitBreakerThreshold = 1;
        config.circuitBreakerResetMs = 50;
    });

    errorIsolation_->registerExternalService("AuthService");
    errorIsolation_->recordExternalServiceFailure("AuthService");

    EXPECT_EQ(CircuitBreakerState::Open,
              errorIsolation_->getCircuitBreakerState("AuthService"));

    // Wait for reset timeout
    std::this_thread::sleep_for(100ms);

    // Check state - should transition to HalfOpen on next check
    EXPECT_TRUE(errorIsolation_->isExternalServiceAvailable("AuthService"));
    EXPECT_EQ(CircuitBreakerState::HalfOpen,
              errorIsolation_->getCircuitBreakerState("AuthService"));
}

TEST_F(ErrorIsolationTest, CircuitBreakerClosesOnSuccess) {
    errorIsolation_->updateConfig([](ErrorIsolationConfig& config) {
        config.circuitBreakerThreshold = 1;
        config.circuitBreakerResetMs = 50;
    });

    errorIsolation_->registerExternalService("AuthService");
    errorIsolation_->recordExternalServiceFailure("AuthService");

    std::this_thread::sleep_for(100ms);

    // Force transition to half-open
    errorIsolation_->isExternalServiceAvailable("AuthService");

    // Record success
    errorIsolation_->recordExternalServiceSuccess("AuthService");

    EXPECT_EQ(CircuitBreakerState::Closed,
              errorIsolation_->getCircuitBreakerState("AuthService"));
}

TEST_F(ErrorIsolationTest, CircuitBreakerReopensOnFailureInHalfOpen) {
    errorIsolation_->updateConfig([](ErrorIsolationConfig& config) {
        config.circuitBreakerThreshold = 1;
        config.circuitBreakerResetMs = 50;
    });

    errorIsolation_->registerExternalService("AuthService");
    errorIsolation_->recordExternalServiceFailure("AuthService");

    std::this_thread::sleep_for(100ms);

    // Force transition to half-open
    errorIsolation_->isExternalServiceAvailable("AuthService");
    EXPECT_EQ(CircuitBreakerState::HalfOpen,
              errorIsolation_->getCircuitBreakerState("AuthService"));

    // Record another failure
    errorIsolation_->recordExternalServiceFailure("AuthService");

    EXPECT_EQ(CircuitBreakerState::Open,
              errorIsolation_->getCircuitBreakerState("AuthService"));
}

// =============================================================================
// Diagnostic Logging Tests (Requirement 20.5)
// =============================================================================

TEST_F(ErrorIsolationTest, UnrecoverableErrorLogged) {
    std::atomic<bool> diagnosticLogged{false};
    std::string loggedDiagnostics;

    errorIsolation_->setDiagnosticLoggingCallback(
        [&](const std::string& diagnostics) {
            diagnosticLogged = true;
            loggedDiagnostics = diagnostics;
        }
    );

    errorIsolation_->reportUnrecoverableError("Critical system failure", "Out of file descriptors");

    EXPECT_TRUE(diagnosticLogged);
    EXPECT_FALSE(loggedDiagnostics.empty());
}

TEST_F(ErrorIsolationTest, DiagnosticsIncludeSystemState) {
    std::string loggedDiagnostics;

    errorIsolation_->setDiagnosticLoggingCallback(
        [&](const std::string& diagnostics) {
            loggedDiagnostics = diagnostics;
        }
    );

    // Create some state
    errorIsolation_->registerConnection(1);
    errorIsolation_->registerConnection(2);
    errorIsolation_->reportMemoryUsage(80, 100);

    errorIsolation_->reportUnrecoverableError("System shutdown", "Resource exhaustion");

    // Diagnostics should include connection count (header is "--- Connections ---")
    EXPECT_NE(std::string::npos, loggedDiagnostics.find("Connections"));
    // Diagnostics should include memory info (header is "--- Memory ---")
    EXPECT_NE(std::string::npos, loggedDiagnostics.find("Memory"));
}

TEST_F(ErrorIsolationTest, DiagnosticsDumpAvailable) {
    errorIsolation_->registerConnection(1);
    errorIsolation_->registerComponent("TestComponent", []() { return true; });
    errorIsolation_->registerExternalService("AuthService");

    std::string dump = errorIsolation_->getDiagnosticDump();

    EXPECT_FALSE(dump.empty());
    // Headers in dump are "--- Connections ---" and "--- Components ---"
    EXPECT_NE(std::string::npos, dump.find("Connections"));
    EXPECT_NE(std::string::npos, dump.find("Components"));
}

// =============================================================================
// Health Check API Tests (Requirement 20.6)
// =============================================================================

TEST_F(ErrorIsolationTest, HealthCheckReturnsHealthyWhenAllOk) {
    HealthStatus status = errorIsolation_->getHealthStatus();

    EXPECT_EQ(HealthState::Healthy, status.state);
    EXPECT_TRUE(status.details.empty() || status.details == "OK");
}

TEST_F(ErrorIsolationTest, HealthCheckReturnsDegradedOnMemoryPressure) {
    errorIsolation_->reportMemoryUsage(95, 100);

    HealthStatus status = errorIsolation_->getHealthStatus();

    EXPECT_EQ(HealthState::Degraded, status.state);
    EXPECT_NE(std::string::npos, status.details.find("memory"));
}

TEST_F(ErrorIsolationTest, HealthCheckReturnsUnhealthyOnCriticalFailure) {
    errorIsolation_->updateConfig([](ErrorIsolationConfig& config) {
        config.componentRestartMaxAttempts = 1;
        config.componentRestartDelayMs = 10;
    });

    errorIsolation_->registerComponent("CriticalComponent",
        []() { return false; }  // Always fails
    );

    errorIsolation_->reportComponentCrash("CriticalComponent", "Fatal error");
    std::this_thread::sleep_for(50ms);

    HealthStatus status = errorIsolation_->getHealthStatus();

    EXPECT_EQ(HealthState::Unhealthy, status.state);
    EXPECT_NE(std::string::npos, status.details.find("CriticalComponent"));
}

TEST_F(ErrorIsolationTest, HealthCheckIncludesComponentStatus) {
    errorIsolation_->registerComponent("MediaHandler", []() { return true; });
    errorIsolation_->registerComponent("AuthHandler", []() { return true; });

    HealthStatus status = errorIsolation_->getHealthStatus();

    EXPECT_EQ(2u, status.componentStatuses.size());
}

TEST_F(ErrorIsolationTest, HealthCheckIncludesCircuitBreakerStatus) {
    errorIsolation_->registerExternalService("AuthService");
    errorIsolation_->registerExternalService("MetricsService");

    HealthStatus status = errorIsolation_->getHealthStatus();

    EXPECT_EQ(2u, status.circuitBreakerStatuses.size());
}

TEST_F(ErrorIsolationTest, HealthCheckIncludesMemoryInfo) {
    errorIsolation_->reportMemoryUsage(80, 100);

    HealthStatus status = errorIsolation_->getHealthStatus();

    EXPECT_EQ(80u, status.memoryUsedBytes);
    EXPECT_EQ(100u, status.memoryTotalBytes);
}

TEST_F(ErrorIsolationTest, HealthCheckIncludesConnectionInfo) {
    errorIsolation_->registerConnection(1);
    errorIsolation_->registerConnection(2);

    HealthStatus status = errorIsolation_->getHealthStatus();

    EXPECT_EQ(2u, status.activeConnections);
}

// =============================================================================
// Statistics Tests
// =============================================================================

TEST_F(ErrorIsolationTest, StatisticsInitiallyZero) {
    auto stats = errorIsolation_->getStatistics();

    EXPECT_EQ(0u, stats.connectionFailures);
    EXPECT_EQ(0u, stats.memoryAllocationFailures);
    EXPECT_EQ(0u, stats.componentCrashes);
    EXPECT_EQ(0u, stats.componentRestarts);
    EXPECT_EQ(0u, stats.circuitBreakerTrips);
    EXPECT_EQ(0u, stats.unrecoverableErrors);
}

TEST_F(ErrorIsolationTest, StatisticsAggregated) {
    // Create various error conditions
    errorIsolation_->registerConnection(1);
    errorIsolation_->handleConnectionError(1, ErrorCode::ConnectionReset, "Reset");

    errorIsolation_->recordMemoryAllocationFailure("Buffer", 1024);

    errorIsolation_->registerComponent("Test", []() { return true; });
    errorIsolation_->reportComponentCrash("Test", "Crash");

    errorIsolation_->updateConfig([](ErrorIsolationConfig& config) {
        config.circuitBreakerThreshold = 1;
    });
    errorIsolation_->registerExternalService("Service");
    errorIsolation_->recordExternalServiceFailure("Service");

    std::this_thread::sleep_for(50ms);

    auto stats = errorIsolation_->getStatistics();

    EXPECT_EQ(1u, stats.connectionFailures);
    EXPECT_EQ(1u, stats.memoryAllocationFailures);
    EXPECT_GE(stats.componentCrashes, 1u);
    EXPECT_EQ(1u, stats.circuitBreakerTrips);
}

TEST_F(ErrorIsolationTest, StatisticsCanBeReset) {
    errorIsolation_->registerConnection(1);
    errorIsolation_->handleConnectionError(1, ErrorCode::ConnectionReset, "Reset");

    auto stats = errorIsolation_->getStatistics();
    EXPECT_EQ(1u, stats.connectionFailures);

    errorIsolation_->resetStatistics();

    stats = errorIsolation_->getStatistics();
    EXPECT_EQ(0u, stats.connectionFailures);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST_F(ErrorIsolationTest, ConcurrentConnectionRegistration) {
    std::vector<std::thread> threads;
    std::atomic<int> registeredCount{0};

    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([this, i, &registeredCount]() {
            for (int j = 0; j < 100; ++j) {
                ConnectionId id = static_cast<ConnectionId>(i * 100 + j + 1);
                errorIsolation_->registerConnection(id);
                registeredCount++;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(1000, registeredCount);
    EXPECT_EQ(1000u, errorIsolation_->connectionCount());
}

TEST_F(ErrorIsolationTest, ConcurrentHealthCheck) {
    std::vector<std::thread> threads;
    std::atomic<int> healthCheckCount{0};

    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([this, &healthCheckCount]() {
            for (int j = 0; j < 100; ++j) {
                auto status = errorIsolation_->getHealthStatus();
                (void)status;  // Suppress unused warning
                healthCheckCount++;
            }
        });
    }

    // Concurrently modify state
    std::thread modifierThread([this]() {
        for (int i = 0; i < 50; ++i) {
            errorIsolation_->reportMemoryUsage(i, 100);
            std::this_thread::sleep_for(1ms);
        }
    });

    for (auto& t : threads) {
        t.join();
    }
    modifierThread.join();

    EXPECT_EQ(1000, healthCheckCount);
}

// =============================================================================
// Configuration Tests
// =============================================================================

TEST_F(ErrorIsolationTest, ConfigurationCanBeUpdated) {
    errorIsolation_->updateConfig([](ErrorIsolationConfig& config) {
        config.memoryThresholdPercent = 80;
    });

    auto config = errorIsolation_->getConfig();
    EXPECT_EQ(80u, config.memoryThresholdPercent);
}

TEST_F(ErrorIsolationTest, ConfigurationValidation) {
    // Memory threshold should be capped
    errorIsolation_->updateConfig([](ErrorIsolationConfig& config) {
        config.memoryThresholdPercent = 150;  // Invalid, should be capped to 100
    });

    auto config = errorIsolation_->getConfig();
    EXPECT_LE(config.memoryThresholdPercent, 100u);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(ErrorIsolationTest, HandleErrorForUnregisteredConnection) {
    // Should not crash when handling error for unregistered connection
    EXPECT_NO_THROW(
        errorIsolation_->handleConnectionError(999, ErrorCode::Unknown, "Unknown connection")
    );
}

TEST_F(ErrorIsolationTest, CrashReportForUnregisteredComponent) {
    // Should not crash when reporting crash for unregistered component
    EXPECT_NO_THROW(
        errorIsolation_->reportComponentCrash("UnknownComponent", "Crash")
    );
}

TEST_F(ErrorIsolationTest, ServiceFailureForUnregisteredService) {
    // Should not crash when recording failure for unregistered service
    EXPECT_NO_THROW(
        errorIsolation_->recordExternalServiceFailure("UnknownService")
    );
}

TEST_F(ErrorIsolationTest, NullCallbacksHandled) {
    errorIsolation_->setConnectionErrorCallback(nullptr);
    errorIsolation_->setMemoryPressureCallback(nullptr);
    errorIsolation_->setDiagnosticLoggingCallback(nullptr);

    errorIsolation_->registerConnection(1);

    // Should not crash with null callbacks
    EXPECT_NO_THROW(
        errorIsolation_->handleConnectionError(1, ErrorCode::Unknown, "Error")
    );
    EXPECT_NO_THROW(
        errorIsolation_->reportMemoryUsage(95, 100)
    );
    EXPECT_NO_THROW(
        errorIsolation_->reportUnrecoverableError("Error", "Details")
    );
}

} // namespace
} // namespace core
} // namespace openrtmp
