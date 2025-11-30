// OpenRTMP - Cross-platform RTMP Server
// Tests for Connection Pool and Limits
//
// Tests cover:
// - Pre-allocate connection objects to avoid allocation during accept
// - Support 1000 concurrent connections on desktop platforms
// - Support 100 concurrent connections on mobile platforms
// - Reject new connections with appropriate error when limits reached
// - Log connection limit events for capacity planning
//
// Requirements coverage:
// - Requirement 14.1: Desktop platforms support 1000 concurrent TCP connections
// - Requirement 14.2: Mobile platforms support 100 concurrent TCP connections
// - Requirement 14.5: Connection pooling for efficient resource management
// - Requirement 14.6: Reject new connections with error when limits reached

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <cstdint>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <condition_variable>

#include "openrtmp/core/connection_pool.hpp"
#include "openrtmp/core/types.hpp"
#include "openrtmp/core/error_codes.hpp"

namespace openrtmp {
namespace core {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class ConnectionPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create pool with small limit for testing
        ConnectionPoolConfig config;
        config.maxConnections = 10;
        config.preAllocateCount = 5;
        pool_ = std::make_unique<ConnectionPool>(config);
    }

    void TearDown() override {
        pool_.reset();
    }

    std::unique_ptr<ConnectionPool> pool_;

    // Helper to acquire multiple connections
    std::vector<ConnectionHandle> acquireMultiple(size_t count) {
        std::vector<ConnectionHandle> handles;
        for (size_t i = 0; i < count; ++i) {
            auto result = pool_->acquire();
            if (result.isSuccess()) {
                handles.push_back(result.value());
            }
        }
        return handles;
    }
};

// =============================================================================
// Configuration Tests
// =============================================================================

TEST_F(ConnectionPoolTest, DefaultDesktopLimit) {
    // Default desktop limit should be 1000 when using platformDefault()
    if (isDesktopPlatform()) {
        ConnectionPoolConfig defaultConfig = ConnectionPoolConfig::platformDefault();
        EXPECT_EQ(defaultConfig.maxConnections, 1000u);
    }
}

TEST_F(ConnectionPoolTest, DefaultMobileLimit) {
    // Default mobile limit should be 100 when using platformDefault()
    if (isMobilePlatform()) {
        ConnectionPoolConfig defaultConfig = ConnectionPoolConfig::platformDefault();
        EXPECT_EQ(defaultConfig.maxConnections, 100u);
    }
}

TEST_F(ConnectionPoolTest, ConfigurationWithCustomLimit) {
    ConnectionPoolConfig config;
    config.maxConnections = 50;
    config.preAllocateCount = 10;

    ConnectionPool customPool(config);
    auto poolConfig = customPool.getConfig();

    EXPECT_EQ(poolConfig.maxConnections, 50u);
    EXPECT_EQ(poolConfig.preAllocateCount, 10u);
}

TEST_F(ConnectionPoolTest, PreAllocationCountCannotExceedMaxConnections) {
    ConnectionPoolConfig config;
    config.maxConnections = 10;
    config.preAllocateCount = 100;  // Exceeds max

    ConnectionPool pool(config);
    auto poolConfig = pool.getConfig();

    // Pre-allocation should be clamped to max
    EXPECT_LE(poolConfig.preAllocateCount, poolConfig.maxConnections);
}

// =============================================================================
// Pre-allocation Tests (Requirement 14.5)
// =============================================================================

TEST_F(ConnectionPoolTest, PreAllocatesConnectionObjects) {
    ConnectionPoolConfig config;
    config.maxConnections = 100;
    config.preAllocateCount = 50;

    ConnectionPool pool(config);
    auto stats = pool.getStatistics();

    // Should have pre-allocated objects available
    EXPECT_GE(stats.availableConnections, 50u);
}

TEST_F(ConnectionPoolTest, PreAllocatedObjectsAreReusable) {
    // Acquire a connection
    auto result1 = pool_->acquire();
    ASSERT_TRUE(result1.isSuccess());
    ConnectionHandle handle1 = result1.value();

    // Release it
    pool_->release(handle1);

    // Acquire again - should get a recycled connection
    auto result2 = pool_->acquire();
    ASSERT_TRUE(result2.isSuccess());

    // Pool should not have grown unnecessarily
    auto stats = pool_->getStatistics();
    EXPECT_LE(stats.totalAllocatedConnections, pool_->getConfig().preAllocateCount + 1);
}

TEST_F(ConnectionPoolTest, AcquireDoesNotAllocateDuringAccept) {
    // Pre-allocate all connections
    ConnectionPoolConfig config;
    config.maxConnections = 10;
    config.preAllocateCount = 10;

    ConnectionPool pool(config);
    auto statsBefore = pool.getStatistics();
    size_t allocsBefore = statsBefore.totalAllocations;

    // Acquire a connection - should use pre-allocated
    auto result = pool.acquire();
    ASSERT_TRUE(result.isSuccess());

    auto statsAfter = pool.getStatistics();
    // Should not have caused a new allocation
    EXPECT_EQ(statsAfter.totalAllocations, allocsBefore);
}

// =============================================================================
// Acquire and Release Tests
// =============================================================================

TEST_F(ConnectionPoolTest, AcquireReturnsValidHandle) {
    auto result = pool_->acquire();

    ASSERT_TRUE(result.isSuccess());
    EXPECT_NE(result.value(), INVALID_CONNECTION_HANDLE);
}

TEST_F(ConnectionPoolTest, AcquireUpdatesStatistics) {
    auto statsBefore = pool_->getStatistics();
    size_t inUseBefore = statsBefore.connectionsInUse;

    auto result = pool_->acquire();
    ASSERT_TRUE(result.isSuccess());

    auto statsAfter = pool_->getStatistics();
    EXPECT_EQ(statsAfter.connectionsInUse, inUseBefore + 1);
}

TEST_F(ConnectionPoolTest, ReleaseReturnsConnectionToPool) {
    auto result = pool_->acquire();
    ASSERT_TRUE(result.isSuccess());
    ConnectionHandle handle = result.value();

    auto statsBefore = pool_->getStatistics();
    size_t inUseBefore = statsBefore.connectionsInUse;

    pool_->release(handle);

    auto statsAfter = pool_->getStatistics();
    EXPECT_EQ(statsAfter.connectionsInUse, inUseBefore - 1);
    EXPECT_GT(statsAfter.availableConnections, statsBefore.availableConnections);
}

TEST_F(ConnectionPoolTest, ReleaseInvalidHandleIsIgnored) {
    // Release an invalid handle - should not crash or throw
    pool_->release(INVALID_CONNECTION_HANDLE);

    auto stats = pool_->getStatistics();
    EXPECT_EQ(stats.connectionsInUse, 0u);
}

TEST_F(ConnectionPoolTest, DoubleReleaseIsIgnored) {
    auto result = pool_->acquire();
    ASSERT_TRUE(result.isSuccess());
    ConnectionHandle handle = result.value();

    pool_->release(handle);
    pool_->release(handle);  // Second release should be ignored

    auto stats = pool_->getStatistics();
    EXPECT_EQ(stats.connectionsInUse, 0u);
}

// =============================================================================
// Connection Limit Tests (Requirements 14.1, 14.2, 14.6)
// =============================================================================

TEST_F(ConnectionPoolTest, RejectsConnectionsWhenLimitReached) {
    // Acquire all available connections
    auto handles = acquireMultiple(10);
    EXPECT_EQ(handles.size(), 10u);

    // Try to acquire one more - should fail
    auto result = pool_->acquire();

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, ErrorCode::ConnectionLimitReached);
}

TEST_F(ConnectionPoolTest, AcceptsConnectionsAfterRelease) {
    // Fill the pool
    auto handles = acquireMultiple(10);
    EXPECT_EQ(handles.size(), 10u);

    // Release one
    pool_->release(handles.back());
    handles.pop_back();

    // Should be able to acquire again
    auto result = pool_->acquire();
    ASSERT_TRUE(result.isSuccess());
}

TEST_F(ConnectionPoolTest, ConnectionLimitErrorIncludesCurrentCount) {
    auto handles = acquireMultiple(10);

    auto result = pool_->acquire();

    ASSERT_TRUE(result.isError());
    EXPECT_FALSE(result.error().message.empty());
    // Message should indicate the limit was reached
    EXPECT_NE(result.error().message.find("limit"), std::string::npos);
}

// =============================================================================
// Statistics and Monitoring Tests
// =============================================================================

TEST_F(ConnectionPoolTest, TracksAvailableConnections) {
    auto statsBefore = pool_->getStatistics();
    size_t availableBefore = statsBefore.availableConnections;

    auto result = pool_->acquire();
    ASSERT_TRUE(result.isSuccess());

    auto statsAfter = pool_->getStatistics();
    EXPECT_EQ(statsAfter.availableConnections, availableBefore - 1);
}

TEST_F(ConnectionPoolTest, TracksConnectionsInUse) {
    auto handles = acquireMultiple(5);

    auto stats = pool_->getStatistics();
    EXPECT_EQ(stats.connectionsInUse, 5u);
}

TEST_F(ConnectionPoolTest, TracksPeakConnectionCount) {
    // Acquire 7 connections
    auto handles = acquireMultiple(7);

    // Release 4
    for (size_t i = 0; i < 4; ++i) {
        pool_->release(handles.back());
        handles.pop_back();
    }

    // Peak should still be 7
    auto stats = pool_->getStatistics();
    EXPECT_EQ(stats.peakConnectionsInUse, 7u);
}

TEST_F(ConnectionPoolTest, TracksRejectedConnectionCount) {
    // Fill the pool
    auto handles = acquireMultiple(10);

    // Try to acquire more - should be rejected
    for (int i = 0; i < 5; ++i) {
        pool_->acquire();
    }

    auto stats = pool_->getStatistics();
    EXPECT_EQ(stats.rejectedConnections, 5u);
}

TEST_F(ConnectionPoolTest, GetStatisticsIsThreadSafe) {
    std::atomic<bool> running{true};
    std::atomic<size_t> statsReadCount{0};
    std::vector<std::thread> threads;

    // Reader threads
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([this, &running, &statsReadCount]() {
            while (running) {
                pool_->getStatistics();
                ++statsReadCount;
            }
        });
    }

    // Writer thread
    threads.emplace_back([this, &running]() {
        while (running) {
            auto result = pool_->acquire();
            if (result.isSuccess()) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                pool_->release(result.value());
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_GT(statsReadCount.load(), 0u);
}

// =============================================================================
// Logging Callback Tests
// =============================================================================

TEST_F(ConnectionPoolTest, InvokesCallbackOnLimitReached) {
    bool limitReachedInvoked = false;
    ConnectionLimitEvent capturedEvent;

    pool_->setConnectionLimitCallback([&](const ConnectionLimitEvent& event) {
        // Capture the LimitReached event specifically
        if (event.type == ConnectionLimitEventType::LimitReached) {
            limitReachedInvoked = true;
            capturedEvent = event;
        }
    });

    // Fill the pool
    auto handles = acquireMultiple(10);

    // Try to acquire one more - should trigger callback
    pool_->acquire();

    EXPECT_TRUE(limitReachedInvoked);
    EXPECT_EQ(capturedEvent.type, ConnectionLimitEventType::LimitReached);
    EXPECT_EQ(capturedEvent.currentCount, 10u);
    EXPECT_EQ(capturedEvent.maxCount, 10u);
}

TEST_F(ConnectionPoolTest, InvokesCallbackOnHighWatermark) {
    bool callbackInvoked = false;
    ConnectionLimitEvent capturedEvent;

    ConnectionPoolConfig config;
    config.maxConnections = 10;
    config.preAllocateCount = 5;
    config.highWatermarkPercent = 80;  // 80% = 8 connections

    ConnectionPool pool(config);
    pool.setConnectionLimitCallback([&](const ConnectionLimitEvent& event) {
        if (event.type == ConnectionLimitEventType::HighWatermark) {
            callbackInvoked = true;
            capturedEvent = event;
        }
    });

    // Acquire 8 connections (80% threshold)
    for (int i = 0; i < 8; ++i) {
        pool.acquire();
    }

    EXPECT_TRUE(callbackInvoked);
    EXPECT_EQ(capturedEvent.type, ConnectionLimitEventType::HighWatermark);
}

TEST_F(ConnectionPoolTest, InvokesCallbackOnLowWatermarkRecovery) {
    bool callbackInvoked = false;
    ConnectionLimitEvent capturedEvent;

    ConnectionPoolConfig config;
    config.maxConnections = 10;
    config.preAllocateCount = 5;
    config.highWatermarkPercent = 80;
    config.lowWatermarkPercent = 50;

    ConnectionPool pool(config);
    pool.setConnectionLimitCallback([&](const ConnectionLimitEvent& event) {
        if (event.type == ConnectionLimitEventType::LowWatermark) {
            callbackInvoked = true;
            capturedEvent = event;
        }
    });

    // Acquire 8 connections (above high watermark)
    std::vector<ConnectionHandle> handles;
    for (int i = 0; i < 8; ++i) {
        auto result = pool.acquire();
        if (result.isSuccess()) {
            handles.push_back(result.value());
        }
    }

    // Release to below low watermark (50% = 5)
    while (handles.size() > 4) {
        pool.release(handles.back());
        handles.pop_back();
    }

    EXPECT_TRUE(callbackInvoked);
    EXPECT_EQ(capturedEvent.type, ConnectionLimitEventType::LowWatermark);
}

// =============================================================================
// Connection Object Access Tests
// =============================================================================

TEST_F(ConnectionPoolTest, GetConnectionReturnsValidPointer) {
    auto result = pool_->acquire();
    ASSERT_TRUE(result.isSuccess());
    ConnectionHandle handle = result.value();

    Connection* conn = pool_->getConnection(handle);
    EXPECT_NE(conn, nullptr);
}

TEST_F(ConnectionPoolTest, GetConnectionReturnsNullForInvalidHandle) {
    Connection* conn = pool_->getConnection(INVALID_CONNECTION_HANDLE);
    EXPECT_EQ(conn, nullptr);
}

TEST_F(ConnectionPoolTest, GetConnectionReturnsNullForReleasedHandle) {
    auto result = pool_->acquire();
    ASSERT_TRUE(result.isSuccess());
    ConnectionHandle handle = result.value();

    pool_->release(handle);

    Connection* conn = pool_->getConnection(handle);
    EXPECT_EQ(conn, nullptr);
}

TEST_F(ConnectionPoolTest, ConnectionObjectIsReset) {
    // Acquire and modify a connection
    auto result1 = pool_->acquire();
    ASSERT_TRUE(result1.isSuccess());
    ConnectionHandle handle1 = result1.value();

    Connection* conn1 = pool_->getConnection(handle1);
    ASSERT_NE(conn1, nullptr);

    // Simulate setting some state
    conn1->clientIP = "192.168.1.100";
    conn1->state = ConnectionState::Connected;

    // Release and re-acquire
    pool_->release(handle1);
    auto result2 = pool_->acquire();
    ASSERT_TRUE(result2.isSuccess());

    Connection* conn2 = pool_->getConnection(result2.value());
    ASSERT_NE(conn2, nullptr);

    // Connection should be reset
    EXPECT_TRUE(conn2->clientIP.empty());
    EXPECT_EQ(conn2->state, ConnectionState::Connecting);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST_F(ConnectionPoolTest, ConcurrentAcquireAndRelease) {
    ConnectionPoolConfig config;
    config.maxConnections = 100;
    config.preAllocateCount = 50;

    ConnectionPool pool(config);

    std::atomic<bool> running{true};
    std::atomic<size_t> successCount{0};
    std::atomic<size_t> failCount{0};
    std::vector<std::thread> threads;

    // Multiple acquire/release threads
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&pool, &running, &successCount, &failCount]() {
            while (running) {
                auto result = pool.acquire();
                if (result.isSuccess()) {
                    ++successCount;
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    pool.release(result.value());
                } else {
                    ++failCount;
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    running = false;

    for (auto& t : threads) {
        t.join();
    }

    // Should have had successful acquires
    EXPECT_GT(successCount.load(), 0u);

    // Final state should be consistent
    auto stats = pool.getStatistics();
    EXPECT_EQ(stats.connectionsInUse, 0u);
}

TEST_F(ConnectionPoolTest, NoDataRaceOnStatistics) {
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;

    // Writer threads
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([this, &running]() {
            while (running) {
                auto result = pool_->acquire();
                if (result.isSuccess()) {
                    pool_->release(result.value());
                }
            }
        });
    }

    // Reader thread checking statistics
    threads.emplace_back([this, &running]() {
        while (running) {
            auto stats = pool_->getStatistics();
            // Statistics should always be consistent
            EXPECT_LE(stats.connectionsInUse, stats.totalAllocatedConnections);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& t : threads) {
        t.join();
    }
}

// =============================================================================
// Platform-Aware Limit Tests
// =============================================================================

TEST(ConnectionPoolPlatformTest, PlatformAwareLimits) {
    ConnectionPoolConfig config = ConnectionPoolConfig::platformDefault();

    if (isDesktopPlatform()) {
        // Desktop: 1000 connections (Requirement 14.1)
        EXPECT_EQ(config.maxConnections, 1000u);
    } else if (isMobilePlatform()) {
        // Mobile: 100 connections (Requirement 14.2)
        EXPECT_EQ(config.maxConnections, 100u);
    }
}

TEST(ConnectionPoolPlatformTest, CanOverridePlatformDefaults) {
    ConnectionPoolConfig config = ConnectionPoolConfig::platformDefault();
    config.maxConnections = 500;  // Override default

    ConnectionPool pool(config);
    auto poolConfig = pool.getConfig();

    EXPECT_EQ(poolConfig.maxConnections, 500u);
}

// =============================================================================
// Reset and Clear Tests
// =============================================================================

TEST_F(ConnectionPoolTest, ClearReleasesAllConnections) {
    auto handles = acquireMultiple(5);
    EXPECT_EQ(pool_->getStatistics().connectionsInUse, 5u);

    pool_->clear();

    auto stats = pool_->getStatistics();
    EXPECT_EQ(stats.connectionsInUse, 0u);
}

TEST_F(ConnectionPoolTest, ClearPreservesConfiguration) {
    auto configBefore = pool_->getConfig();

    pool_->clear();

    auto configAfter = pool_->getConfig();
    EXPECT_EQ(configAfter.maxConnections, configBefore.maxConnections);
    EXPECT_EQ(configAfter.preAllocateCount, configBefore.preAllocateCount);
}

TEST_F(ConnectionPoolTest, ResetStatisticsWorks) {
    auto handles = acquireMultiple(5);
    for (auto& h : handles) {
        pool_->release(h);
    }

    pool_->resetStatistics();

    auto stats = pool_->getStatistics();
    EXPECT_EQ(stats.peakConnectionsInUse, 0u);
    EXPECT_EQ(stats.rejectedConnections, 0u);
    EXPECT_EQ(stats.totalAcquires, 0u);
    EXPECT_EQ(stats.totalReleases, 0u);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(ConnectionPoolTest, ZeroMaxConnectionsIsInvalid) {
    ConnectionPoolConfig config;
    config.maxConnections = 0;

    // Should use minimum of 1
    ConnectionPool pool(config);
    auto poolConfig = pool.getConfig();

    EXPECT_GE(poolConfig.maxConnections, 1u);
}

TEST_F(ConnectionPoolTest, HandlesRapidAcquireRelease) {
    // Rapid acquire/release cycles
    for (int i = 0; i < 1000; ++i) {
        auto result = pool_->acquire();
        if (result.isSuccess()) {
            pool_->release(result.value());
        }
    }

    auto stats = pool_->getStatistics();
    EXPECT_EQ(stats.connectionsInUse, 0u);
}

} // namespace test
} // namespace core
} // namespace openrtmp
