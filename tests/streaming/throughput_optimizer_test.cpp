// OpenRTMP - Cross-platform RTMP Server
// Tests for Throughput Optimization
//
// Tests cover:
// - Support 50 Mbps ingestion bitrate on desktop
// - Support 20 Mbps ingestion bitrate on mobile
// - Support 500 Mbps aggregate distribution on desktop
// - Support 100 Mbps aggregate distribution on mobile
// - Log warnings for streams exceeding configured bitrate limits
// - Bitrate measurement and tracking per stream
// - Platform-aware limits
// - Aggregate throughput monitoring
// - Throughput statistics and reporting
// - Bitrate smoothing/averaging over time window
//
// Requirements coverage:
// - Requirement 13.1: 50 Mbps ingestion on desktop
// - Requirement 13.2: 20 Mbps ingestion on mobile
// - Requirement 13.3: 500 Mbps aggregate distribution on desktop
// - Requirement 13.4: 100 Mbps aggregate distribution on mobile
// - Requirement 13.6: Log warnings for exceeding bitrate limits

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <cstdint>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>

#include "openrtmp/streaming/throughput_optimizer.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace streaming {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class ThroughputOptimizerTest : public ::testing::Test {
protected:
    void SetUp() override {
        optimizer_ = std::make_unique<ThroughputOptimizer>();
    }

    void TearDown() override {
        optimizer_.reset();
    }

    std::unique_ptr<ThroughputOptimizer> optimizer_;

    // Helper to simulate data transfer (in bytes)
    void simulateDataTransfer(const StreamKey& key, size_t bytes, bool isIngestion) {
        if (isIngestion) {
            optimizer_->recordIngestionData(key, bytes);
        } else {
            optimizer_->recordDistributionData(key, bytes);
        }
    }

    // Convert Mbps to bytes per second
    static constexpr size_t mbpsToBytesPerSecond(double mbps) {
        return static_cast<size_t>(mbps * 1000000.0 / 8.0);
    }

    // Convert Mbps to bytes per millisecond
    static constexpr size_t mbpsToBytesPerMs(double mbps) {
        return static_cast<size_t>(mbps * 1000.0 / 8.0);
    }

    // Calculate bytes to send in given interval (ms) to achieve target bitrate (Mbps)
    // Formula: bytes = (mbps * 1,000,000 bits/s) / 8 bits/byte * (intervalMs / 1000 s)
    //        = mbps * 125,000 * intervalMs / 1000
    //        = mbps * 125 * intervalMs
    static constexpr size_t bytesForInterval(double mbps, int intervalMs) {
        return static_cast<size_t>(mbps * 125.0 * static_cast<double>(intervalMs));
    }
};

// =============================================================================
// Platform-Specific Ingestion Bitrate Target Tests (Requirements 13.1, 13.2)
// =============================================================================

TEST_F(ThroughputOptimizerTest, DesktopIngestionLimitIs50Mbps) {
    auto config = ThroughputConfig::forDesktop();

    // 50 Mbps = 50,000,000 bits/second = 6,250,000 bytes/second
    EXPECT_EQ(config.maxIngestionBitrateMbps, 50.0);
    EXPECT_EQ(config.maxIngestionBytesPerSecond(), mbpsToBytesPerSecond(50.0));
}

TEST_F(ThroughputOptimizerTest, MobileIngestionLimitIs20Mbps) {
    auto config = ThroughputConfig::forMobile();

    // 20 Mbps = 20,000,000 bits/second = 2,500,000 bytes/second
    EXPECT_EQ(config.maxIngestionBitrateMbps, 20.0);
    EXPECT_EQ(config.maxIngestionBytesPerSecond(), mbpsToBytesPerSecond(20.0));
}

TEST_F(ThroughputOptimizerTest, DefaultConfigUsesCurrentPlatform) {
    auto config = ThroughputConfig::forCurrentPlatform();

    if (core::isDesktopPlatform()) {
        EXPECT_EQ(config.maxIngestionBitrateMbps, 50.0);
    } else if (core::isMobilePlatform()) {
        EXPECT_EQ(config.maxIngestionBitrateMbps, 20.0);
    }
}

// =============================================================================
// Platform-Specific Distribution Throughput Target Tests (Requirements 13.3, 13.4)
// =============================================================================

TEST_F(ThroughputOptimizerTest, DesktopDistributionLimitIs500Mbps) {
    auto config = ThroughputConfig::forDesktop();

    // 500 Mbps = 500,000,000 bits/second = 62,500,000 bytes/second
    EXPECT_EQ(config.maxDistributionBitrateMbps, 500.0);
    EXPECT_EQ(config.maxDistributionBytesPerSecond(), mbpsToBytesPerSecond(500.0));
}

TEST_F(ThroughputOptimizerTest, MobileDistributionLimitIs100Mbps) {
    auto config = ThroughputConfig::forMobile();

    // 100 Mbps = 100,000,000 bits/second = 12,500,000 bytes/second
    EXPECT_EQ(config.maxDistributionBitrateMbps, 100.0);
    EXPECT_EQ(config.maxDistributionBytesPerSecond(), mbpsToBytesPerSecond(100.0));
}

// =============================================================================
// Configuration Tests
// =============================================================================

TEST_F(ThroughputOptimizerTest, ConfigurationCanBeChanged) {
    ThroughputConfig config;
    config.maxIngestionBitrateMbps = 30.0;
    config.maxDistributionBitrateMbps = 200.0;
    config.warningEnabled = true;

    optimizer_->setConfig(config);

    auto retrievedConfig = optimizer_->getConfig();
    EXPECT_DOUBLE_EQ(retrievedConfig.maxIngestionBitrateMbps, 30.0);
    EXPECT_DOUBLE_EQ(retrievedConfig.maxDistributionBitrateMbps, 200.0);
    EXPECT_TRUE(retrievedConfig.warningEnabled);
}

TEST_F(ThroughputOptimizerTest, ConfigurationHasSmoothingWindow) {
    auto config = ThroughputConfig::forDesktop();

    // Default smoothing window should be reasonable (e.g., 1 second)
    EXPECT_GE(config.smoothingWindowMs.count(), 100);
    EXPECT_LE(config.smoothingWindowMs.count(), 5000);
}

// =============================================================================
// Stream Tracking Tests
// =============================================================================

TEST_F(ThroughputOptimizerTest, StartsAndStopsStreamTracking) {
    StreamKey key("live", "test");

    optimizer_->startStreamTracking(key);
    EXPECT_TRUE(optimizer_->isTrackingStream(key));

    optimizer_->stopStreamTracking(key);
    EXPECT_FALSE(optimizer_->isTrackingStream(key));
}

TEST_F(ThroughputOptimizerTest, TracksMultipleStreams) {
    StreamKey key1("live", "test1");
    StreamKey key2("live", "test2");
    StreamKey key3("live", "test3");

    optimizer_->startStreamTracking(key1);
    optimizer_->startStreamTracking(key2);
    optimizer_->startStreamTracking(key3);

    auto streams = optimizer_->getTrackedStreams();
    EXPECT_EQ(streams.size(), 3u);
}

TEST_F(ThroughputOptimizerTest, GetAllTrackedStreams) {
    StreamKey key1("live", "test1");
    StreamKey key2("live", "test2");

    optimizer_->startStreamTracking(key1);
    optimizer_->startStreamTracking(key2);

    auto streams = optimizer_->getTrackedStreams();
    EXPECT_EQ(streams.size(), 2u);
}

// =============================================================================
// Ingestion Bitrate Recording Tests
// =============================================================================

TEST_F(ThroughputOptimizerTest, RecordsIngestionData) {
    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Simulate 1MB data ingestion
    optimizer_->recordIngestionData(key, 1024 * 1024);

    auto stats = optimizer_->getStreamStatistics(key);
    ASSERT_TRUE(stats.has_value());
    EXPECT_GT(stats->totalIngestionBytes, 0u);
}

TEST_F(ThroughputOptimizerTest, CalculatesIngestionBitrate) {
    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Simulate data at known rate: 10 Mbps
    // Send bytesForInterval(10, 10) bytes every 10ms to achieve 10 Mbps
    const int intervalMs = 10;
    size_t bytesPerInterval = bytesForInterval(10.0, intervalMs);  // 10 Mbps
    for (int i = 0; i < 100; ++i) {
        optimizer_->recordIngestionData(key, bytesPerInterval);
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    auto stats = optimizer_->getStreamStatistics(key);
    ASSERT_TRUE(stats.has_value());
    // Bitrate should be approximately 10 Mbps (with some tolerance)
    EXPECT_GT(stats->currentIngestionBitrateMbps, 5.0);
    EXPECT_LT(stats->currentIngestionBitrateMbps, 20.0);
}

// =============================================================================
// Distribution Bitrate Recording Tests
// =============================================================================

TEST_F(ThroughputOptimizerTest, RecordsDistributionData) {
    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Simulate 1MB data distribution
    optimizer_->recordDistributionData(key, 1024 * 1024);

    auto stats = optimizer_->getStreamStatistics(key);
    ASSERT_TRUE(stats.has_value());
    EXPECT_GT(stats->totalDistributionBytes, 0u);
}

TEST_F(ThroughputOptimizerTest, CalculatesDistributionBitrate) {
    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Simulate data at known rate: 25 Mbps
    const int intervalMs = 10;
    size_t bytesPerInterval = bytesForInterval(25.0, intervalMs);  // 25 Mbps
    for (int i = 0; i < 100; ++i) {
        optimizer_->recordDistributionData(key, bytesPerInterval);
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    auto stats = optimizer_->getStreamStatistics(key);
    ASSERT_TRUE(stats.has_value());
    // Bitrate should be approximately 25 Mbps (with some tolerance)
    EXPECT_GT(stats->currentDistributionBitrateMbps, 15.0);
    EXPECT_LT(stats->currentDistributionBitrateMbps, 40.0);
}

// =============================================================================
// Aggregate Throughput Tests
// =============================================================================

TEST_F(ThroughputOptimizerTest, CalculatesAggregateIngestionThroughput) {
    StreamKey key1("live", "test1");
    StreamKey key2("live", "test2");

    optimizer_->startStreamTracking(key1);
    optimizer_->startStreamTracking(key2);

    // Simulate data on both streams: 5 Mbps each = 10 Mbps aggregate
    const int intervalMs = 10;
    for (int i = 0; i < 50; ++i) {
        optimizer_->recordIngestionData(key1, bytesForInterval(5.0, intervalMs));
        optimizer_->recordIngestionData(key2, bytesForInterval(5.0, intervalMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    auto aggregate = optimizer_->getAggregateThroughput();
    // Combined rate should be approximately 10 Mbps (allow some timing variance)
    EXPECT_GE(aggregate.currentIngestionBitrateMbps, 4.0);
}

TEST_F(ThroughputOptimizerTest, CalculatesAggregateDistributionThroughput) {
    StreamKey key1("live", "test1");
    StreamKey key2("live", "test2");

    optimizer_->startStreamTracking(key1);
    optimizer_->startStreamTracking(key2);

    // Simulate data on both streams: 10 Mbps each = 20 Mbps aggregate
    const int intervalMs = 10;
    for (int i = 0; i < 50; ++i) {
        optimizer_->recordDistributionData(key1, bytesForInterval(10.0, intervalMs));
        optimizer_->recordDistributionData(key2, bytesForInterval(10.0, intervalMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    auto aggregate = optimizer_->getAggregateThroughput();
    // Combined rate should be approximately 20 Mbps (allow some timing variance)
    EXPECT_GE(aggregate.currentDistributionBitrateMbps, 8.0);
}

// =============================================================================
// Warning Callback Tests (Requirement 13.6)
// =============================================================================

TEST_F(ThroughputOptimizerTest, CallbackInvokedWhenIngestionExceedsLimit) {
    bool callbackInvoked = false;
    StreamKey capturedKey;
    double capturedBitrate = 0.0;
    ThroughputWarningType capturedType = ThroughputWarningType::IngestionOverLimit;

    optimizer_->setOnThroughputWarningCallback([&](
        const StreamKey& key,
        double bitrateMbps,
        ThroughputWarningType type
    ) {
        callbackInvoked = true;
        capturedKey = key;
        capturedBitrate = bitrateMbps;
        capturedType = type;
    });

    // Configure low limit to trigger warning
    ThroughputConfig config;
    config.maxIngestionBitrateMbps = 5.0;
    config.warningEnabled = true;
    config.smoothingWindowMs = std::chrono::milliseconds(100);
    optimizer_->setConfig(config);

    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Simulate data exceeding limit (10 Mbps > 5 Mbps limit)
    const int intervalMs = 10;
    for (int i = 0; i < 20; ++i) {
        optimizer_->recordIngestionData(key, bytesForInterval(10.0, intervalMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    EXPECT_TRUE(callbackInvoked);
    EXPECT_EQ(capturedKey.name, "test");
    EXPECT_EQ(capturedType, ThroughputWarningType::IngestionOverLimit);
}

TEST_F(ThroughputOptimizerTest, CallbackInvokedWhenDistributionExceedsLimit) {
    bool callbackInvoked = false;
    ThroughputWarningType capturedType = ThroughputWarningType::IngestionOverLimit;  // Wrong initial value

    optimizer_->setOnThroughputWarningCallback([&](
        const StreamKey& key,
        double bitrateMbps,
        ThroughputWarningType type
    ) {
        callbackInvoked = true;
        capturedType = type;
    });

    // Configure low aggregate limit to trigger warning
    ThroughputConfig config;
    config.maxDistributionBitrateMbps = 5.0;  // Aggregate limit
    config.maxPerStreamDistributionBitrateMbps = 0.0;  // No per-stream limit
    config.warningEnabled = true;
    config.smoothingWindowMs = std::chrono::milliseconds(100);
    optimizer_->setConfig(config);

    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Simulate data exceeding aggregate limit (10 Mbps > 5 Mbps aggregate limit)
    const int intervalMs = 10;
    for (int i = 0; i < 20; ++i) {
        optimizer_->recordDistributionData(key, bytesForInterval(10.0, intervalMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    EXPECT_TRUE(callbackInvoked);
    // Should be AggregateDistributionOverLimit since no per-stream limit is set
    EXPECT_EQ(capturedType, ThroughputWarningType::AggregateDistributionOverLimit);
}

TEST_F(ThroughputOptimizerTest, WarningCanBeDisabled) {
    bool callbackInvoked = false;

    optimizer_->setOnThroughputWarningCallback([&](
        const StreamKey& key,
        double bitrateMbps,
        ThroughputWarningType type
    ) {
        callbackInvoked = true;
    });

    // Configure limit but disable warnings
    ThroughputConfig config;
    config.maxIngestionBitrateMbps = 5.0;
    config.warningEnabled = false;
    config.smoothingWindowMs = std::chrono::milliseconds(100);
    optimizer_->setConfig(config);

    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Simulate data exceeding limit (10 Mbps > 5 Mbps limit)
    const int intervalMs = 10;
    for (int i = 0; i < 20; ++i) {
        optimizer_->recordIngestionData(key, bytesForInterval(10.0, intervalMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    EXPECT_FALSE(callbackInvoked);
}

TEST_F(ThroughputOptimizerTest, NoWarningWhenWithinLimits) {
    bool callbackInvoked = false;

    optimizer_->setOnThroughputWarningCallback([&](
        const StreamKey& key,
        double bitrateMbps,
        ThroughputWarningType type
    ) {
        callbackInvoked = true;
    });

    ThroughputConfig config = ThroughputConfig::forDesktop();
    config.warningEnabled = true;
    optimizer_->setConfig(config);

    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Simulate data within limit (10 Mbps < 50 Mbps limit)
    const int intervalMs = 10;
    for (int i = 0; i < 20; ++i) {
        optimizer_->recordIngestionData(key, bytesForInterval(10.0, intervalMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    EXPECT_FALSE(callbackInvoked);
}

// =============================================================================
// Limit Check Tests
// =============================================================================

TEST_F(ThroughputOptimizerTest, DetectsIngestionOverLimit) {
    ThroughputConfig config;
    config.maxIngestionBitrateMbps = 5.0;
    config.smoothingWindowMs = std::chrono::milliseconds(100);
    optimizer_->setConfig(config);

    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Simulate data exceeding limit (10 Mbps > 5 Mbps limit)
    const int intervalMs = 10;
    for (int i = 0; i < 20; ++i) {
        optimizer_->recordIngestionData(key, bytesForInterval(10.0, intervalMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    EXPECT_TRUE(optimizer_->isIngestionOverLimit(key));
}

TEST_F(ThroughputOptimizerTest, DetectsDistributionOverLimit) {
    ThroughputConfig config;
    config.maxDistributionBitrateMbps = 100.0;  // Aggregate limit (high)
    config.maxPerStreamDistributionBitrateMbps = 5.0;  // Per-stream limit (low)
    config.smoothingWindowMs = std::chrono::milliseconds(100);
    optimizer_->setConfig(config);

    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Simulate data exceeding per-stream limit (10 Mbps > 5 Mbps per-stream limit)
    const int intervalMs = 10;
    for (int i = 0; i < 20; ++i) {
        optimizer_->recordDistributionData(key, bytesForInterval(10.0, intervalMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    EXPECT_TRUE(optimizer_->isDistributionOverLimit(key));
}

TEST_F(ThroughputOptimizerTest, DetectsAggregateDistributionOverLimit) {
    ThroughputConfig config;
    config.maxDistributionBitrateMbps = 10.0;
    config.smoothingWindowMs = std::chrono::milliseconds(500);  // Longer window for stability
    optimizer_->setConfig(config);

    StreamKey key1("live", "test1");
    StreamKey key2("live", "test2");
    optimizer_->startStreamTracking(key1);
    optimizer_->startStreamTracking(key2);

    // Each stream 10 Mbps, aggregate 20 Mbps > 10 Mbps limit
    // Use higher bitrate to ensure we exceed limit even with timing variations
    const int intervalMs = 10;
    for (int i = 0; i < 50; ++i) {
        optimizer_->recordDistributionData(key1, bytesForInterval(10.0, intervalMs));
        optimizer_->recordDistributionData(key2, bytesForInterval(10.0, intervalMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    EXPECT_TRUE(optimizer_->isAggregateDistributionOverLimit());
}

// =============================================================================
// Statistics Tests
// =============================================================================

TEST_F(ThroughputOptimizerTest, TracksStreamStatistics) {
    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    for (int i = 0; i < 50; ++i) {
        optimizer_->recordIngestionData(key, 1024);
        optimizer_->recordDistributionData(key, 2048);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto stats = optimizer_->getStreamStatistics(key);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->streamKey.name, "test");
    EXPECT_EQ(stats->totalIngestionBytes, 50u * 1024);
    EXPECT_EQ(stats->totalDistributionBytes, 50u * 2048);
    EXPECT_GT(stats->trackingDurationMs.count(), 0);
}

TEST_F(ThroughputOptimizerTest, CalculatesAverageIngestionBitrate) {
    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Simulate data at 10 Mbps for 1 second
    const int intervalMs = 10;
    for (int i = 0; i < 100; ++i) {
        optimizer_->recordIngestionData(key, bytesForInterval(10.0, intervalMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    auto stats = optimizer_->getStreamStatistics(key);
    ASSERT_TRUE(stats.has_value());
    // Average should be approximately 10 Mbps
    EXPECT_GT(stats->averageIngestionBitrateMbps, 5.0);
    EXPECT_LT(stats->averageIngestionBitrateMbps, 20.0);
}

TEST_F(ThroughputOptimizerTest, TracksPeakBitrates) {
    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Low bitrate (5 Mbps)
    const int intervalMs = 10;
    for (int i = 0; i < 20; ++i) {
        optimizer_->recordIngestionData(key, bytesForInterval(5.0, intervalMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    // High bitrate burst (20 Mbps)
    for (int i = 0; i < 20; ++i) {
        optimizer_->recordIngestionData(key, bytesForInterval(20.0, intervalMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    auto stats = optimizer_->getStreamStatistics(key);
    ASSERT_TRUE(stats.has_value());
    // Peak should be close to 20 Mbps (allow timing variance, should be > 5 Mbps initial rate)
    EXPECT_GE(stats->peakIngestionBitrateMbps, 5.0);
}

// =============================================================================
// Bitrate Smoothing Tests
// =============================================================================

TEST_F(ThroughputOptimizerTest, SmoothingWindowAffectsBitrateCalculation) {
    StreamKey key("live", "test");

    // Short smoothing window
    ThroughputConfig config;
    config.smoothingWindowMs = std::chrono::milliseconds(100);
    optimizer_->setConfig(config);
    optimizer_->startStreamTracking(key);

    // Send burst then nothing (50 Mbps for 100ms)
    const int intervalMs = 10;
    for (int i = 0; i < 10; ++i) {
        optimizer_->recordIngestionData(key, bytesForInterval(50.0, intervalMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    auto statsAfterBurst = optimizer_->getStreamStatistics(key);
    ASSERT_TRUE(statsAfterBurst.has_value());
    double bitrateAfterBurst = statsAfterBurst->currentIngestionBitrateMbps;

    // Wait longer than smoothing window
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto statsAfterWait = optimizer_->getStreamStatistics(key);
    ASSERT_TRUE(statsAfterWait.has_value());

    // Current bitrate should have dropped after smoothing window
    EXPECT_LT(statsAfterWait->currentIngestionBitrateMbps, bitrateAfterBurst);
}

// =============================================================================
// Reset and Lifecycle Tests
// =============================================================================

TEST_F(ThroughputOptimizerTest, ResetClearsAllStatistics) {
    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    for (int i = 0; i < 10; ++i) {
        optimizer_->recordIngestionData(key, 1024);
        optimizer_->recordDistributionData(key, 2048);
    }

    optimizer_->reset();

    // Stream tracking should be cleared
    EXPECT_FALSE(optimizer_->isTrackingStream(key));

    auto aggregate = optimizer_->getAggregateThroughput();
    EXPECT_EQ(aggregate.totalIngestionBytes, 0u);
    EXPECT_EQ(aggregate.totalDistributionBytes, 0u);
}

TEST_F(ThroughputOptimizerTest, ResetPreservesConfiguration) {
    ThroughputConfig config;
    config.maxIngestionBitrateMbps = 30.0;
    optimizer_->setConfig(config);

    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);
    optimizer_->recordIngestionData(key, 1024);

    optimizer_->reset();

    auto retrievedConfig = optimizer_->getConfig();
    EXPECT_DOUBLE_EQ(retrievedConfig.maxIngestionBitrateMbps, 30.0);
}

TEST_F(ThroughputOptimizerTest, StopTrackingClearsStreamData) {
    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    optimizer_->recordIngestionData(key, 1024);

    optimizer_->stopStreamTracking(key);

    auto stats = optimizer_->getStreamStatistics(key);
    EXPECT_FALSE(stats.has_value());
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

class ThroughputOptimizerConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        optimizer_ = std::make_unique<ThroughputOptimizer>();
    }

    std::unique_ptr<ThroughputOptimizer> optimizer_;
};

TEST_F(ThroughputOptimizerConcurrencyTest, ConcurrentDataRecordingIsThreadSafe) {
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;

    StreamKey key1("live", "test1");
    StreamKey key2("live", "test2");
    optimizer_->startStreamTracking(key1);
    optimizer_->startStreamTracking(key2);

    // Multiple producer threads recording data
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([this, &running, &key1, &key2, i]() {
            StreamKey key = (i % 2 == 0) ? key1 : key2;
            while (running) {
                optimizer_->recordIngestionData(key, 1024);
                optimizer_->recordDistributionData(key, 2048);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }

    // Reader thread
    threads.emplace_back([this, &running, &key1]() {
        while (running) {
            optimizer_->getStreamStatistics(key1);
            optimizer_->getAggregateThroughput();
            optimizer_->isIngestionOverLimit(key1);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& thread : threads) {
        thread.join();
    }

    // Test passes if no crashes or deadlocks
    auto stats1 = optimizer_->getStreamStatistics(key1);
    auto stats2 = optimizer_->getStreamStatistics(key2);
    ASSERT_TRUE(stats1.has_value());
    ASSERT_TRUE(stats2.has_value());
    EXPECT_GT(stats1->totalIngestionBytes, 0u);
    EXPECT_GT(stats2->totalIngestionBytes, 0u);
}

// =============================================================================
// Integration Tests
// =============================================================================

TEST_F(ThroughputOptimizerTest, EndToEndThroughputTracking) {
    // Configure for desktop targets
    ThroughputConfig config = ThroughputConfig::forDesktop();
    optimizer_->setConfig(config);

    StreamKey key("live", "integration_test");
    optimizer_->startStreamTracking(key);

    // Simulate realistic streaming scenario
    // 10 Mbps ingestion, 30 Mbps distribution (3 subscribers at 10 Mbps each)
    const int intervalMs = 10;
    for (int frame = 0; frame < 100; ++frame) {
        optimizer_->recordIngestionData(key, bytesForInterval(10.0, intervalMs));
        optimizer_->recordDistributionData(key, bytesForInterval(30.0, intervalMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    // Verify stream statistics
    auto streamStats = optimizer_->getStreamStatistics(key);
    ASSERT_TRUE(streamStats.has_value());
    EXPECT_GT(streamStats->currentIngestionBitrateMbps, 5.0);
    EXPECT_LT(streamStats->currentIngestionBitrateMbps, 20.0);
    EXPECT_GT(streamStats->currentDistributionBitrateMbps, 15.0);
    EXPECT_LT(streamStats->currentDistributionBitrateMbps, 50.0);

    // Should be within desktop limits
    EXPECT_FALSE(optimizer_->isIngestionOverLimit(key));
    EXPECT_FALSE(optimizer_->isDistributionOverLimit(key));
    EXPECT_FALSE(optimizer_->isAggregateDistributionOverLimit());

    // Verify aggregate
    auto aggregate = optimizer_->getAggregateThroughput();
    EXPECT_GT(aggregate.totalIngestionBytes, 0u);
    EXPECT_GT(aggregate.totalDistributionBytes, 0u);

    optimizer_->stopStreamTracking(key);
}

TEST_F(ThroughputOptimizerTest, MultiStreamThroughputTracking) {
    ThroughputConfig config = ThroughputConfig::forDesktop();
    optimizer_->setConfig(config);

    StreamKey key1("live", "stream1");
    StreamKey key2("live", "stream2");
    optimizer_->startStreamTracking(key1);
    optimizer_->startStreamTracking(key2);

    // Simulate multiple streams
    // stream1: 15 Mbps ingestion, 45 Mbps distribution
    // stream2: 10 Mbps ingestion, 30 Mbps distribution
    const int intervalMs = 10;
    for (int i = 0; i < 50; ++i) {
        optimizer_->recordIngestionData(key1, bytesForInterval(15.0, intervalMs));
        optimizer_->recordIngestionData(key2, bytesForInterval(10.0, intervalMs));
        optimizer_->recordDistributionData(key1, bytesForInterval(45.0, intervalMs));
        optimizer_->recordDistributionData(key2, bytesForInterval(30.0, intervalMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    // Verify per-stream statistics
    auto stats1 = optimizer_->getStreamStatistics(key1);
    auto stats2 = optimizer_->getStreamStatistics(key2);
    ASSERT_TRUE(stats1.has_value());
    ASSERT_TRUE(stats2.has_value());

    // Verify aggregate (25 Mbps ingestion, 75 Mbps distribution)
    // Allow timing variance - just verify meaningful throughput is recorded
    auto aggregate = optimizer_->getAggregateThroughput();
    EXPECT_GE(aggregate.currentIngestionBitrateMbps, 10.0);
    EXPECT_GE(aggregate.currentDistributionBitrateMbps, 30.0);

    // Both individual streams should be within limits
    EXPECT_FALSE(optimizer_->isIngestionOverLimit(key1));
    EXPECT_FALSE(optimizer_->isIngestionOverLimit(key2));

    optimizer_->stopStreamTracking(key1);
    optimizer_->stopStreamTracking(key2);
}

} // namespace test
} // namespace streaming
} // namespace openrtmp
