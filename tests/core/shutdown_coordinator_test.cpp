// OpenRTMP - Cross-platform RTMP Server
// Shutdown Coordinator Tests
//
// TDD test file for graceful shutdown implementation.
// Tests written before implementation per TDD methodology.

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>

#include "openrtmp/core/shutdown_coordinator.hpp"

namespace openrtmp {
namespace core {
namespace {

using namespace std::chrono_literals;

// =============================================================================
// Test Fixtures
// =============================================================================

class ShutdownCoordinatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        coordinator_ = std::make_unique<ShutdownCoordinator>();
    }

    void TearDown() override {
        coordinator_.reset();
    }

    std::unique_ptr<ShutdownCoordinator> coordinator_;
};

// =============================================================================
// Shutdown State Machine Tests
// =============================================================================

TEST_F(ShutdownCoordinatorTest, InitialStateIsRunning) {
    EXPECT_EQ(ShutdownState::Running, coordinator_->state());
    EXPECT_FALSE(coordinator_->isShuttingDown());
    EXPECT_FALSE(coordinator_->isShutdownComplete());
}

TEST_F(ShutdownCoordinatorTest, InitiateShutdownTransitionsToStoppingNewConnections) {
    auto result = coordinator_->initiateShutdown();
    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(ShutdownState::StoppingNewConnections, coordinator_->state());
    EXPECT_TRUE(coordinator_->isShuttingDown());
}

TEST_F(ShutdownCoordinatorTest, InitiateShutdownFailsWhenAlreadyShuttingDown) {
    coordinator_->initiateShutdown();
    auto result = coordinator_->initiateShutdown();
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(ShutdownError::Code::AlreadyShuttingDown, result.error().code);
}

TEST_F(ShutdownCoordinatorTest, StateProgressesThroughShutdownPhases) {
    coordinator_->initiateShutdown();

    // Should progress through states
    coordinator_->notifyPublishersComplete();
    EXPECT_EQ(ShutdownState::NotifyingPublishers, coordinator_->state());

    coordinator_->gracePeriodComplete();
    EXPECT_EQ(ShutdownState::GracePeriod, coordinator_->state());

    coordinator_->forceTerminationComplete();
    EXPECT_EQ(ShutdownState::ForceTerminating, coordinator_->state());

    coordinator_->shutdownComplete();
    EXPECT_EQ(ShutdownState::Complete, coordinator_->state());
    EXPECT_TRUE(coordinator_->isShutdownComplete());
}

// =============================================================================
// Connection Acceptance Blocking Tests (Requirement 19.1)
// =============================================================================

TEST_F(ShutdownCoordinatorTest, AcceptingConnectionsWhenRunning) {
    EXPECT_TRUE(coordinator_->canAcceptConnection());
}

TEST_F(ShutdownCoordinatorTest, RejectsNewConnectionsAfterShutdownInitiated) {
    coordinator_->initiateShutdown();
    EXPECT_FALSE(coordinator_->canAcceptConnection());
}

TEST_F(ShutdownCoordinatorTest, RejectsNewConnectionsDuringAllShutdownPhases) {
    coordinator_->initiateShutdown();
    EXPECT_FALSE(coordinator_->canAcceptConnection());

    coordinator_->notifyPublishersComplete();
    EXPECT_FALSE(coordinator_->canAcceptConnection());

    coordinator_->gracePeriodComplete();
    EXPECT_FALSE(coordinator_->canAcceptConnection());
}

// =============================================================================
// Publisher Notification Tests (Requirement 19.2)
// =============================================================================

TEST_F(ShutdownCoordinatorTest, PublisherNotificationCallbackInvoked) {
    std::atomic<int> notificationCount{0};
    std::vector<PublisherId> notifiedPublishers;
    std::mutex notifyMutex;

    coordinator_->setPublisherNotificationCallback(
        [&](PublisherId publisherId) {
            std::lock_guard<std::mutex> lock(notifyMutex);
            notificationCount++;
            notifiedPublishers.push_back(publisherId);
        }
    );

    // Register some publishers
    coordinator_->registerPublisher(1);
    coordinator_->registerPublisher(2);
    coordinator_->registerPublisher(3);

    coordinator_->initiateShutdown();

    // Allow some time for notifications
    std::this_thread::sleep_for(50ms);

    EXPECT_EQ(3, notificationCount);
    EXPECT_EQ(3u, notifiedPublishers.size());
}

TEST_F(ShutdownCoordinatorTest, PublisherCanBeUnregistered) {
    coordinator_->registerPublisher(1);
    coordinator_->registerPublisher(2);
    coordinator_->unregisterPublisher(1);

    EXPECT_EQ(1u, coordinator_->publisherCount());
}

TEST_F(ShutdownCoordinatorTest, NoNotificationForUnregisteredPublisher) {
    std::vector<PublisherId> notifiedPublishers;
    std::mutex notifyMutex;

    coordinator_->setPublisherNotificationCallback(
        [&](PublisherId publisherId) {
            std::lock_guard<std::mutex> lock(notifyMutex);
            notifiedPublishers.push_back(publisherId);
        }
    );

    coordinator_->registerPublisher(1);
    coordinator_->registerPublisher(2);
    coordinator_->unregisterPublisher(1);

    coordinator_->initiateShutdown();
    std::this_thread::sleep_for(50ms);

    EXPECT_EQ(1u, notifiedPublishers.size());
    EXPECT_EQ(2u, notifiedPublishers[0]);
}

// =============================================================================
// Grace Period Tests (Requirement 19.3, 19.4)
// =============================================================================

TEST_F(ShutdownCoordinatorTest, DefaultGracePeriodIs30Seconds) {
    EXPECT_EQ(30000u, coordinator_->gracePeriodMs());
}

TEST_F(ShutdownCoordinatorTest, GracePeriodCanBeConfigured) {
    coordinator_->setGracePeriodMs(5000);
    EXPECT_EQ(5000u, coordinator_->gracePeriodMs());
}

TEST_F(ShutdownCoordinatorTest, GracePeriodCallbackInvokedAfterTimeout) {
    std::atomic<bool> gracePeriodExpired{false};

    coordinator_->setGracePeriodExpiredCallback([&]() {
        gracePeriodExpired = true;
    });

    // Use short grace period for testing
    coordinator_->setGracePeriodMs(100);

    coordinator_->initiateShutdown();
    coordinator_->startGracePeriod();

    // Wait for grace period to expire
    std::this_thread::sleep_for(200ms);

    EXPECT_TRUE(gracePeriodExpired);
}

TEST_F(ShutdownCoordinatorTest, ActiveStreamsTrackingDuringGracePeriod) {
    coordinator_->registerActiveStream("stream1");
    coordinator_->registerActiveStream("stream2");
    EXPECT_EQ(2u, coordinator_->activeStreamCount());

    coordinator_->initiateShutdown();

    // Streams complete during grace period
    coordinator_->unregisterActiveStream("stream1");
    EXPECT_EQ(1u, coordinator_->activeStreamCount());
}

TEST_F(ShutdownCoordinatorTest, EarlyCompletionWhenAllStreamsEnd) {
    std::atomic<bool> allStreamsEnded{false};

    coordinator_->setAllStreamsEndedCallback([&]() {
        allStreamsEnded = true;
    });

    coordinator_->registerActiveStream("stream1");
    coordinator_->registerActiveStream("stream2");

    coordinator_->initiateShutdown();
    coordinator_->startGracePeriod();

    // All streams end before grace period expires
    coordinator_->unregisterActiveStream("stream1");
    coordinator_->unregisterActiveStream("stream2");

    std::this_thread::sleep_for(50ms);

    EXPECT_TRUE(allStreamsEnded);
}

// =============================================================================
// Force Shutdown Tests (Requirement 19.5)
// =============================================================================

TEST_F(ShutdownCoordinatorTest, ForceShutdownImmediatelyTerminates) {
    coordinator_->registerPublisher(1);
    coordinator_->registerActiveStream("stream1");

    auto result = coordinator_->forceShutdown();
    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(ShutdownState::ForceTerminating, coordinator_->state());
}

TEST_F(ShutdownCoordinatorTest, ForceShutdownCallbackInvoked) {
    std::atomic<bool> forceShutdownCalled{false};

    coordinator_->setForceShutdownCallback([&]() {
        forceShutdownCalled = true;
    });

    coordinator_->forceShutdown();

    EXPECT_TRUE(forceShutdownCalled);
}

TEST_F(ShutdownCoordinatorTest, ForceShutdownSkipsGracePeriod) {
    std::atomic<bool> gracePeriodStarted{false};

    coordinator_->setGracePeriodExpiredCallback([&]() {
        gracePeriodStarted = true;
    });

    coordinator_->forceShutdown();

    std::this_thread::sleep_for(50ms);

    // Grace period should not have been started
    EXPECT_FALSE(gracePeriodStarted);
    EXPECT_NE(ShutdownState::GracePeriod, coordinator_->state());
}

TEST_F(ShutdownCoordinatorTest, ForceShutdownDuringGracefulShutdown) {
    coordinator_->setGracePeriodMs(10000);  // Long grace period

    coordinator_->initiateShutdown();
    coordinator_->startGracePeriod();

    // Force shutdown during grace period
    auto result = coordinator_->forceShutdown();
    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(ShutdownState::ForceTerminating, coordinator_->state());
}

// =============================================================================
// Shutdown Statistics and Logging Tests (Requirement 19.6)
// =============================================================================

TEST_F(ShutdownCoordinatorTest, ShutdownStatisticsInitiallyZero) {
    auto stats = coordinator_->getShutdownStatistics();

    EXPECT_EQ(0u, stats.totalConnectionsTerminated);
    EXPECT_EQ(0u, stats.totalStreamsInterrupted);
    EXPECT_EQ(0u, stats.gracefulDisconnects);
    EXPECT_EQ(0u, stats.forcedDisconnects);
}

TEST_F(ShutdownCoordinatorTest, ConnectionTerminationTracked) {
    coordinator_->recordConnectionTermination(true);  // Graceful
    coordinator_->recordConnectionTermination(false); // Forced
    coordinator_->recordConnectionTermination(true);  // Graceful

    auto stats = coordinator_->getShutdownStatistics();

    EXPECT_EQ(3u, stats.totalConnectionsTerminated);
    EXPECT_EQ(2u, stats.gracefulDisconnects);
    EXPECT_EQ(1u, stats.forcedDisconnects);
}

TEST_F(ShutdownCoordinatorTest, StreamInterruptionTracked) {
    coordinator_->recordStreamInterruption();
    coordinator_->recordStreamInterruption();

    auto stats = coordinator_->getShutdownStatistics();
    EXPECT_EQ(2u, stats.totalStreamsInterrupted);
}

TEST_F(ShutdownCoordinatorTest, ShutdownSummaryProvided) {
    coordinator_->registerPublisher(1);
    coordinator_->registerPublisher(2);
    coordinator_->registerActiveStream("stream1");

    coordinator_->recordConnectionTermination(true);
    coordinator_->recordConnectionTermination(false);
    coordinator_->recordStreamInterruption();

    auto summary = coordinator_->getShutdownSummary();

    EXPECT_FALSE(summary.empty());
    // Summary should contain connection count information
    EXPECT_NE(std::string::npos, summary.find("connection"));
    // Summary should contain stream count information
    EXPECT_NE(std::string::npos, summary.find("stream"));
}

TEST_F(ShutdownCoordinatorTest, ShutdownDurationTracked) {
    coordinator_->initiateShutdown();

    std::this_thread::sleep_for(100ms);

    coordinator_->shutdownComplete();

    auto stats = coordinator_->getShutdownStatistics();
    EXPECT_GE(stats.shutdownDurationMs, 100u);
}

// =============================================================================
// Connection Tracking Tests
// =============================================================================

TEST_F(ShutdownCoordinatorTest, ConnectionCountTracking) {
    coordinator_->registerConnection(100);
    coordinator_->registerConnection(101);
    coordinator_->registerConnection(102);

    EXPECT_EQ(3u, coordinator_->connectionCount());

    coordinator_->unregisterConnection(101);
    EXPECT_EQ(2u, coordinator_->connectionCount());
}

TEST_F(ShutdownCoordinatorTest, ForceTerminateCallbackForRemainingConnections) {
    std::vector<ConnectionId> terminatedConnections;
    std::mutex mutex;

    coordinator_->setConnectionForceTerminateCallback(
        [&](ConnectionId connId) {
            std::lock_guard<std::mutex> lock(mutex);
            terminatedConnections.push_back(connId);
        }
    );

    coordinator_->registerConnection(100);
    coordinator_->registerConnection(101);
    coordinator_->registerConnection(102);

    coordinator_->forceShutdown();
    coordinator_->terminateAllConnections();

    std::this_thread::sleep_for(50ms);

    EXPECT_EQ(3u, terminatedConnections.size());
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST_F(ShutdownCoordinatorTest, ConcurrentPublisherRegistration) {
    std::vector<std::thread> threads;

    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([this, i]() {
            for (int j = 0; j < 100; ++j) {
                PublisherId id = static_cast<PublisherId>(i * 100 + j);
                coordinator_->registerPublisher(id);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(1000u, coordinator_->publisherCount());
}

TEST_F(ShutdownCoordinatorTest, ConcurrentStateAccess) {
    std::atomic<bool> startTest{false};
    std::atomic<int> readCount{0};

    std::vector<std::thread> readers;

    // Start multiple readers
    for (int i = 0; i < 5; ++i) {
        readers.emplace_back([this, &startTest, &readCount]() {
            while (!startTest) {
                std::this_thread::yield();
            }
            for (int j = 0; j < 100; ++j) {
                coordinator_->state();
                coordinator_->isShuttingDown();
                coordinator_->canAcceptConnection();
                readCount++;
            }
        });
    }

    startTest = true;

    // Initiate shutdown while reading
    coordinator_->initiateShutdown();

    for (auto& t : readers) {
        t.join();
    }

    EXPECT_EQ(500, readCount);
}

// =============================================================================
// Callback Safety Tests
// =============================================================================

TEST_F(ShutdownCoordinatorTest, NullCallbacksAreHandled) {
    // Should not crash with null callbacks
    coordinator_->setPublisherNotificationCallback(nullptr);
    coordinator_->setGracePeriodExpiredCallback(nullptr);
    coordinator_->setForceShutdownCallback(nullptr);

    coordinator_->registerPublisher(1);
    coordinator_->initiateShutdown();

    // Should complete without crashing
    EXPECT_TRUE(coordinator_->isShuttingDown());
}

TEST_F(ShutdownCoordinatorTest, CallbackExceptionDoesNotCrash) {
    coordinator_->setPublisherNotificationCallback(
        [](PublisherId) {
            throw std::runtime_error("Test exception");
        }
    );

    coordinator_->registerPublisher(1);

    // Should not crash even if callback throws
    EXPECT_NO_THROW(coordinator_->initiateShutdown());
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(ShutdownCoordinatorTest, ShutdownWithNoConnections) {
    auto result = coordinator_->initiateShutdown();
    EXPECT_TRUE(result.isSuccess());

    auto stats = coordinator_->getShutdownStatistics();
    EXPECT_EQ(0u, stats.totalConnectionsTerminated);
}

TEST_F(ShutdownCoordinatorTest, ShutdownWithNoPublishers) {
    std::atomic<int> notificationCount{0};

    coordinator_->setPublisherNotificationCallback(
        [&](PublisherId) {
            notificationCount++;
        }
    );

    coordinator_->initiateShutdown();
    std::this_thread::sleep_for(50ms);

    EXPECT_EQ(0, notificationCount);
}

TEST_F(ShutdownCoordinatorTest, MultipleForceShutdownCalls) {
    auto result1 = coordinator_->forceShutdown();
    EXPECT_TRUE(result1.isSuccess());

    auto result2 = coordinator_->forceShutdown();
    EXPECT_TRUE(result2.isError());
    EXPECT_EQ(ShutdownError::Code::AlreadyShuttingDown, result2.error().code);
}

TEST_F(ShutdownCoordinatorTest, StateStringConversion) {
    EXPECT_STREQ("Running", shutdownStateToString(ShutdownState::Running));
    EXPECT_STREQ("StoppingNewConnections", shutdownStateToString(ShutdownState::StoppingNewConnections));
    EXPECT_STREQ("NotifyingPublishers", shutdownStateToString(ShutdownState::NotifyingPublishers));
    EXPECT_STREQ("GracePeriod", shutdownStateToString(ShutdownState::GracePeriod));
    EXPECT_STREQ("ForceTerminating", shutdownStateToString(ShutdownState::ForceTerminating));
    EXPECT_STREQ("Complete", shutdownStateToString(ShutdownState::Complete));
}

} // namespace
} // namespace core
} // namespace openrtmp
