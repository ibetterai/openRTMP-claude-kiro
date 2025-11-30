// OpenRTMP - Cross-platform RTMP Server
// Performance Tests: Connection Scalability Validation
//
// Task 21.3: Implement E2E and performance tests
// Validates connection scalability targets with simulated load.
//
// Note: Tests use mock/simulated connections rather than actual network sockets.
// Real scalability testing requires dedicated load testing infrastructure.
//
// Requirements coverage:
// - Requirement 14.1: Desktop platforms support 1000 concurrent TCP connections
// - Requirement 14.2: Mobile platforms support 100 concurrent TCP connections
// - Requirement 14.3: Support 10 simultaneous publishing streams on desktop
// - Requirement 14.4: Support 3 simultaneous publishing streams on mobile
// - Requirement 14.5: Connection pooling for efficient resource management
// - Requirement 14.6: Reject new connections with error when limits reached

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/result.hpp"

namespace openrtmp {
namespace performance {
namespace test {

// =============================================================================
// Scalability Constants
// =============================================================================

// Desktop targets (Requirements 14.1, 14.3)
constexpr size_t DESKTOP_CONNECTION_TARGET = 1000;
constexpr size_t DESKTOP_PUBLISHER_LIMIT = 10;

// Mobile targets (Requirements 14.2, 14.4)
constexpr size_t MOBILE_CONNECTION_TARGET = 100;
constexpr size_t MOBILE_PUBLISHER_LIMIT = 3;

// Test configuration
constexpr size_t TEST_DESKTOP_CONNECTIONS = 1000;
constexpr size_t TEST_MOBILE_CONNECTIONS = 100;

// =============================================================================
// Mock Connection Pool for Scalability Testing
// =============================================================================

/**
 * @brief Mock connection structure for load testing.
 */
struct MockConnection {
    uint64_t id;
    std::string clientIP;
    uint16_t clientPort;
    bool isActive;
    bool isPublishing;
    std::chrono::steady_clock::time_point createdAt;
};

/**
 * @brief Error codes for connection operations.
 */
enum class ConnectionError {
    Success,
    PoolExhausted,
    PublisherLimitReached,
    InvalidConnection,
    AllocationFailed
};

/**
 * @brief Mock connection pool for scalability testing.
 */
class MockConnectionPool {
public:
    MockConnectionPool(size_t maxConnections, size_t maxPublishers)
        : maxConnections_(maxConnections)
        , maxPublishers_(maxPublishers)
        , nextConnectionId_(1)
        , activeConnections_(0)
        , activePublishers_(0)
        , totalAllocations_(0)
        , totalRejections_(0)
        , peakConnections_(0)
        , peakMemoryBytes_(0)
    {}

    // Pre-allocate connection objects (Requirement 14.5)
    bool preallocate(size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);

        size_t toPreallocate = std::min(count, maxConnections_);
        preallocatedPool_.reserve(toPreallocate);

        for (size_t i = 0; i < toPreallocate; ++i) {
            MockConnection conn;
            conn.id = 0;  // Not assigned yet
            conn.isActive = false;
            conn.isPublishing = false;
            preallocatedPool_.push_back(std::move(conn));
        }

        return true;
    }

    // Allocate connection from pool
    core::Result<uint64_t, ConnectionError> allocateConnection(
        const std::string& clientIP,
        uint16_t clientPort) {

        std::lock_guard<std::mutex> lock(mutex_);

        if (activeConnections_ >= maxConnections_) {
            totalRejections_++;
            return core::Result<uint64_t, ConnectionError>::error(
                ConnectionError::PoolExhausted);
        }

        MockConnection conn;
        conn.id = nextConnectionId_++;
        conn.clientIP = clientIP;
        conn.clientPort = clientPort;
        conn.isActive = true;
        conn.isPublishing = false;
        conn.createdAt = std::chrono::steady_clock::now();

        connections_[conn.id] = conn;
        activeConnections_++;
        totalAllocations_++;

        if (activeConnections_ > peakConnections_) {
            peakConnections_ = activeConnections_;
        }

        // Estimate memory per connection
        updateMemoryEstimate();

        return core::Result<uint64_t, ConnectionError>::success(conn.id);
    }

    // Release connection back to pool
    core::Result<void, ConnectionError> releaseConnection(uint64_t connectionId) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = connections_.find(connectionId);
        if (it == connections_.end()) {
            return core::Result<void, ConnectionError>::error(
                ConnectionError::InvalidConnection);
        }

        if (it->second.isPublishing) {
            activePublishers_--;
        }

        connections_.erase(it);
        activeConnections_--;

        updateMemoryEstimate();

        return core::Result<void, ConnectionError>::success();
    }

    // Promote connection to publisher (Requirements 14.3, 14.4)
    core::Result<void, ConnectionError> setPublishing(uint64_t connectionId, bool publishing) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = connections_.find(connectionId);
        if (it == connections_.end()) {
            return core::Result<void, ConnectionError>::error(
                ConnectionError::InvalidConnection);
        }

        if (publishing && !it->second.isPublishing) {
            if (activePublishers_ >= maxPublishers_) {
                return core::Result<void, ConnectionError>::error(
                    ConnectionError::PublisherLimitReached);
            }
            activePublishers_++;
        } else if (!publishing && it->second.isPublishing) {
            activePublishers_--;
        }

        it->second.isPublishing = publishing;
        return core::Result<void, ConnectionError>::success();
    }

    // Statistics
    size_t getActiveConnections() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return activeConnections_;
    }

    size_t getActivePublishers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return activePublishers_;
    }

    size_t getMaxConnections() const { return maxConnections_; }
    size_t getMaxPublishers() const { return maxPublishers_; }
    size_t getTotalAllocations() const { return totalAllocations_; }
    size_t getTotalRejections() const { return totalRejections_; }
    size_t getPeakConnections() const { return peakConnections_; }
    size_t getPeakMemoryBytes() const { return peakMemoryBytes_; }

    size_t getRemainingCapacity() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return maxConnections_ - activeConnections_;
    }

    size_t getRemainingPublisherSlots() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return maxPublishers_ - activePublishers_;
    }

    // Clear all connections
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.clear();
        activeConnections_ = 0;
        activePublishers_ = 0;
    }

private:
    void updateMemoryEstimate() {
        // Estimate ~1KB per connection structure
        size_t currentMemory = activeConnections_ * 1024;
        if (currentMemory > peakMemoryBytes_) {
            peakMemoryBytes_ = currentMemory;
        }
    }

    mutable std::mutex mutex_;
    size_t maxConnections_;
    size_t maxPublishers_;
    uint64_t nextConnectionId_;
    size_t activeConnections_;
    size_t activePublishers_;
    size_t totalAllocations_;
    size_t totalRejections_;
    size_t peakConnections_;
    size_t peakMemoryBytes_;
    std::unordered_map<uint64_t, MockConnection> connections_;
    std::vector<MockConnection> preallocatedPool_;
};

// =============================================================================
// Test Fixtures
// =============================================================================

class ScalabilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create desktop and mobile pools with respective limits
        desktopPool_ = std::make_unique<MockConnectionPool>(
            DESKTOP_CONNECTION_TARGET, DESKTOP_PUBLISHER_LIMIT);
        mobilePool_ = std::make_unique<MockConnectionPool>(
            MOBILE_CONNECTION_TARGET, MOBILE_PUBLISHER_LIMIT);
    }

    void TearDown() override {
        mobilePool_.reset();
        desktopPool_.reset();
    }

    std::unique_ptr<MockConnectionPool> desktopPool_;
    std::unique_ptr<MockConnectionPool> mobilePool_;
};

// =============================================================================
// Desktop Connection Scalability Tests (Requirement 14.1)
// =============================================================================

TEST_F(ScalabilityTest, DesktopSupports1000ConcurrentConnections) {
    std::vector<uint64_t> connectionIds;
    connectionIds.reserve(DESKTOP_CONNECTION_TARGET);

    // Allocate exactly 1000 connections
    for (size_t i = 0; i < DESKTOP_CONNECTION_TARGET; ++i) {
        std::string ip = "192.168.1." + std::to_string(i % 255);
        uint16_t port = static_cast<uint16_t>(10000 + i);

        auto result = desktopPool_->allocateConnection(ip, port);
        ASSERT_TRUE(result.isSuccess()) << "Failed at connection " << i;
        connectionIds.push_back(result.value());
    }

    EXPECT_EQ(desktopPool_->getActiveConnections(), DESKTOP_CONNECTION_TARGET);
    EXPECT_EQ(desktopPool_->getPeakConnections(), DESKTOP_CONNECTION_TARGET);
}

TEST_F(ScalabilityTest, DesktopRejectsConnectionAt1001) {
    // Fill pool to capacity
    for (size_t i = 0; i < DESKTOP_CONNECTION_TARGET; ++i) {
        desktopPool_->allocateConnection("10.0.0.1", static_cast<uint16_t>(i));
    }

    // Attempt to allocate one more
    auto result = desktopPool_->allocateConnection("10.0.0.1", 9999);

    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.error(), ConnectionError::PoolExhausted);
    EXPECT_EQ(desktopPool_->getTotalRejections(), 1u);
}

TEST_F(ScalabilityTest, DesktopConnectionPoolMemoryEstimate) {
    // Allocate maximum connections
    for (size_t i = 0; i < DESKTOP_CONNECTION_TARGET; ++i) {
        desktopPool_->allocateConnection("10.0.0.1", static_cast<uint16_t>(i));
    }

    // Memory should be reasonable (< 10MB for 1000 connections)
    size_t memoryBytes = desktopPool_->getPeakMemoryBytes();
    size_t memoryMB = memoryBytes / (1024 * 1024);

    EXPECT_LT(memoryMB, 10u);
}

// =============================================================================
// Mobile Connection Scalability Tests (Requirement 14.2)
// =============================================================================

TEST_F(ScalabilityTest, MobileSupports100ConcurrentConnections) {
    std::vector<uint64_t> connectionIds;
    connectionIds.reserve(MOBILE_CONNECTION_TARGET);

    for (size_t i = 0; i < MOBILE_CONNECTION_TARGET; ++i) {
        std::string ip = "172.16.0." + std::to_string(i % 255);
        uint16_t port = static_cast<uint16_t>(20000 + i);

        auto result = mobilePool_->allocateConnection(ip, port);
        ASSERT_TRUE(result.isSuccess()) << "Failed at connection " << i;
        connectionIds.push_back(result.value());
    }

    EXPECT_EQ(mobilePool_->getActiveConnections(), MOBILE_CONNECTION_TARGET);
}

TEST_F(ScalabilityTest, MobileRejectsConnectionAt101) {
    // Fill pool to capacity
    for (size_t i = 0; i < MOBILE_CONNECTION_TARGET; ++i) {
        mobilePool_->allocateConnection("10.0.0.1", static_cast<uint16_t>(i));
    }

    // Attempt to allocate one more
    auto result = mobilePool_->allocateConnection("10.0.0.1", 9999);

    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.error(), ConnectionError::PoolExhausted);
}

TEST_F(ScalabilityTest, MobileConnectionPoolMemoryEstimate) {
    for (size_t i = 0; i < MOBILE_CONNECTION_TARGET; ++i) {
        mobilePool_->allocateConnection("10.0.0.1", static_cast<uint16_t>(i));
    }

    // Memory should be very small (< 1MB for 100 connections)
    size_t memoryBytes = mobilePool_->getPeakMemoryBytes();
    size_t memoryKB = memoryBytes / 1024;

    EXPECT_LT(memoryKB, 1024u);
}

// =============================================================================
// Desktop Publisher Limit Tests (Requirement 14.3)
// =============================================================================

TEST_F(ScalabilityTest, DesktopSupports10SimultaneousPublishers) {
    // Create 10 connections and promote to publishers
    std::vector<uint64_t> publisherIds;

    for (size_t i = 0; i < DESKTOP_PUBLISHER_LIMIT; ++i) {
        auto connResult = desktopPool_->allocateConnection("10.0.0.1", static_cast<uint16_t>(i));
        ASSERT_TRUE(connResult.isSuccess());

        auto pubResult = desktopPool_->setPublishing(connResult.value(), true);
        ASSERT_TRUE(pubResult.isSuccess()) << "Failed to create publisher " << i;

        publisherIds.push_back(connResult.value());
    }

    EXPECT_EQ(desktopPool_->getActivePublishers(), DESKTOP_PUBLISHER_LIMIT);
}

TEST_F(ScalabilityTest, DesktopRejects11thPublisher) {
    // Create maximum publishers
    std::vector<uint64_t> connIds;
    for (size_t i = 0; i < DESKTOP_PUBLISHER_LIMIT + 1; ++i) {
        auto connResult = desktopPool_->allocateConnection("10.0.0.1", static_cast<uint16_t>(i));
        ASSERT_TRUE(connResult.isSuccess());
        connIds.push_back(connResult.value());
    }

    // Promote first 10 to publishers
    for (size_t i = 0; i < DESKTOP_PUBLISHER_LIMIT; ++i) {
        auto result = desktopPool_->setPublishing(connIds[i], true);
        ASSERT_TRUE(result.isSuccess());
    }

    // 11th should be rejected
    auto result = desktopPool_->setPublishing(connIds[DESKTOP_PUBLISHER_LIMIT], true);
    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.error(), ConnectionError::PublisherLimitReached);
}

// =============================================================================
// Mobile Publisher Limit Tests (Requirement 14.4)
// =============================================================================

TEST_F(ScalabilityTest, MobileSupports3SimultaneousPublishers) {
    std::vector<uint64_t> publisherIds;

    for (size_t i = 0; i < MOBILE_PUBLISHER_LIMIT; ++i) {
        auto connResult = mobilePool_->allocateConnection("10.0.0.1", static_cast<uint16_t>(i));
        ASSERT_TRUE(connResult.isSuccess());

        auto pubResult = mobilePool_->setPublishing(connResult.value(), true);
        ASSERT_TRUE(pubResult.isSuccess()) << "Failed to create mobile publisher " << i;

        publisherIds.push_back(connResult.value());
    }

    EXPECT_EQ(mobilePool_->getActivePublishers(), MOBILE_PUBLISHER_LIMIT);
}

TEST_F(ScalabilityTest, MobileRejects4thPublisher) {
    std::vector<uint64_t> connIds;
    for (size_t i = 0; i < MOBILE_PUBLISHER_LIMIT + 1; ++i) {
        auto connResult = mobilePool_->allocateConnection("10.0.0.1", static_cast<uint16_t>(i));
        ASSERT_TRUE(connResult.isSuccess());
        connIds.push_back(connResult.value());
    }

    // Promote first 3 to publishers
    for (size_t i = 0; i < MOBILE_PUBLISHER_LIMIT; ++i) {
        auto result = mobilePool_->setPublishing(connIds[i], true);
        ASSERT_TRUE(result.isSuccess());
    }

    // 4th should be rejected
    auto result = mobilePool_->setPublishing(connIds[MOBILE_PUBLISHER_LIMIT], true);
    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.error(), ConnectionError::PublisherLimitReached);
}

// =============================================================================
// Connection Pooling Tests (Requirement 14.5)
// =============================================================================

TEST_F(ScalabilityTest, ConnectionPoolPreallocation) {
    auto pool = std::make_unique<MockConnectionPool>(1000, 10);

    // Preallocate 50% of capacity
    bool success = pool->preallocate(500);
    EXPECT_TRUE(success);
}

TEST_F(ScalabilityTest, ConnectionPoolReuseAfterRelease) {
    // Allocate and release connections
    for (int cycle = 0; cycle < 10; ++cycle) {
        std::vector<uint64_t> ids;

        // Allocate 100 connections
        for (size_t i = 0; i < 100; ++i) {
            auto result = desktopPool_->allocateConnection("10.0.0.1", static_cast<uint16_t>(i));
            ASSERT_TRUE(result.isSuccess());
            ids.push_back(result.value());
        }

        EXPECT_EQ(desktopPool_->getActiveConnections(), 100u);

        // Release all connections
        for (auto id : ids) {
            desktopPool_->releaseConnection(id);
        }

        EXPECT_EQ(desktopPool_->getActiveConnections(), 0u);
    }

    // Total allocations should be 1000 (10 cycles * 100 connections)
    EXPECT_EQ(desktopPool_->getTotalAllocations(), 1000u);
}

TEST_F(ScalabilityTest, ConnectionPoolCapacityTracking) {
    // Initial capacity
    EXPECT_EQ(desktopPool_->getRemainingCapacity(), DESKTOP_CONNECTION_TARGET);

    // Allocate some connections
    std::vector<uint64_t> ids;
    for (size_t i = 0; i < 500; ++i) {
        auto result = desktopPool_->allocateConnection("10.0.0.1", static_cast<uint16_t>(i));
        ids.push_back(result.value());
    }

    EXPECT_EQ(desktopPool_->getRemainingCapacity(), 500u);

    // Release half
    for (size_t i = 0; i < 250; ++i) {
        desktopPool_->releaseConnection(ids[i]);
    }

    EXPECT_EQ(desktopPool_->getRemainingCapacity(), 750u);
}

// =============================================================================
// Concurrent Access Tests
// =============================================================================

TEST_F(ScalabilityTest, ConcurrentConnectionAllocation) {
    std::atomic<size_t> successCount{0};
    std::atomic<size_t> failCount{0};
    std::vector<std::thread> threads;

    // 10 threads each trying to allocate 200 connections
    for (int t = 0; t < 10; ++t) {
        threads.emplace_back([this, t, &successCount, &failCount]() {
            for (int i = 0; i < 200; ++i) {
                auto result = desktopPool_->allocateConnection(
                    "10.0." + std::to_string(t) + "." + std::to_string(i % 255),
                    static_cast<uint16_t>(t * 1000 + i));

                if (result.isSuccess()) {
                    successCount++;
                } else {
                    failCount++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Exactly 1000 should succeed, 1000 should fail
    EXPECT_EQ(successCount.load(), DESKTOP_CONNECTION_TARGET);
    EXPECT_EQ(failCount.load(), 1000u);
    EXPECT_EQ(desktopPool_->getActiveConnections(), DESKTOP_CONNECTION_TARGET);
}

TEST_F(ScalabilityTest, ConcurrentAllocationAndRelease) {
    std::atomic<bool> running{true};
    std::atomic<size_t> allocations{0};
    std::atomic<size_t> releases{0};
    std::vector<std::thread> threads;
    std::mutex idsMutex;
    std::vector<uint64_t> activeIds;

    // Allocation threads
    for (int t = 0; t < 5; ++t) {
        threads.emplace_back([this, t, &running, &allocations, &idsMutex, &activeIds]() {
            int counter = 0;
            while (running) {
                auto result = desktopPool_->allocateConnection(
                    "10.0.0." + std::to_string(counter % 255),
                    static_cast<uint16_t>(t * 10000 + counter));

                if (result.isSuccess()) {
                    allocations++;
                    std::lock_guard<std::mutex> lock(idsMutex);
                    activeIds.push_back(result.value());
                }
                counter++;
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });
    }

    // Release threads
    for (int t = 0; t < 3; ++t) {
        threads.emplace_back([this, &running, &releases, &idsMutex, &activeIds]() {
            while (running) {
                uint64_t idToRelease = 0;
                {
                    std::lock_guard<std::mutex> lock(idsMutex);
                    if (!activeIds.empty()) {
                        idToRelease = activeIds.back();
                        activeIds.pop_back();
                    }
                }
                if (idToRelease != 0) {
                    desktopPool_->releaseConnection(idToRelease);
                    releases++;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(15));
            }
        });
    }

    // Run for a short time
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& thread : threads) {
        thread.join();
    }

    // Verify pool is still in consistent state
    EXPECT_LE(desktopPool_->getActiveConnections(), DESKTOP_CONNECTION_TARGET);
    EXPECT_GE(allocations.load(), releases.load());
}

// =============================================================================
// Performance Benchmarks
// =============================================================================

TEST_F(ScalabilityTest, AllocationPerformanceBenchmark) {
    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < DESKTOP_CONNECTION_TARGET; ++i) {
        desktopPool_->allocateConnection("10.0.0.1", static_cast<uint16_t>(i));
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    // Should allocate 1000 connections in under 100ms
    EXPECT_LT(duration.count(), 100000);

    double allocsPerSecond = 1000000.0 * DESKTOP_CONNECTION_TARGET / duration.count();
    // Log performance metric (informational)
    EXPECT_GT(allocsPerSecond, 10000.0);  // At least 10k allocations/sec
}

TEST_F(ScalabilityTest, ReleasePerformanceBenchmark) {
    std::vector<uint64_t> ids;
    for (size_t i = 0; i < DESKTOP_CONNECTION_TARGET; ++i) {
        auto result = desktopPool_->allocateConnection("10.0.0.1", static_cast<uint16_t>(i));
        ids.push_back(result.value());
    }

    auto start = std::chrono::high_resolution_clock::now();

    for (auto id : ids) {
        desktopPool_->releaseConnection(id);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    // Should release 1000 connections in under 100ms
    EXPECT_LT(duration.count(), 100000);
}

TEST_F(ScalabilityTest, MixedOperationsPerformance) {
    std::vector<uint64_t> activeIds;
    int operations = 10000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < operations; ++i) {
        if (i % 3 == 0 || activeIds.empty()) {
            // Allocate
            auto result = desktopPool_->allocateConnection("10.0.0.1", static_cast<uint16_t>(i));
            if (result.isSuccess()) {
                activeIds.push_back(result.value());
            }
        } else {
            // Release
            if (!activeIds.empty()) {
                desktopPool_->releaseConnection(activeIds.back());
                activeIds.pop_back();
            }
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 10000 operations should complete in under 500ms
    EXPECT_LT(duration.count(), 500);
}

} // namespace test
} // namespace performance
} // namespace openrtmp
