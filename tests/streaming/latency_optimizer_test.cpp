// OpenRTMP - Cross-platform RTMP Server
// Tests for Latency Optimization
//
// Tests cover:
// - Target glass-to-glass latency under 2 seconds on desktop
// - Target glass-to-glass latency under 3 seconds on mobile
// - Process and forward chunks within 50ms of receipt
// - Support low-latency mode with 500ms maximum subscriber buffer
// - Latency measurement and tracking
// - Platform-aware latency targets
// - Latency statistics and reporting
//
// Requirements coverage:
// - Requirement 12.1: Glass-to-glass latency <2s on desktop
// - Requirement 12.2: Glass-to-glass latency <3s on mobile
// - Requirement 12.3: Process and forward chunks within 50ms
// - Requirement 12.4: Low-latency mode option
// - Requirement 12.5: Low-latency mode 500ms maximum buffer

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <cstdint>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>

#include "openrtmp/streaming/latency_optimizer.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace streaming {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class LatencyOptimizerTest : public ::testing::Test {
protected:
    void SetUp() override {
        optimizer_ = std::make_unique<LatencyOptimizer>();
    }

    void TearDown() override {
        optimizer_.reset();
    }

    std::unique_ptr<LatencyOptimizer> optimizer_;

    // Helper to simulate chunk processing
    void simulateChunkProcessing(uint32_t chunkId,
                                  std::chrono::milliseconds processingTime) {
        optimizer_->recordChunkReceived(chunkId);
        std::this_thread::sleep_for(processingTime);
        optimizer_->recordChunkForwarded(chunkId);
    }
};

// =============================================================================
// Platform-Specific Latency Target Tests
// =============================================================================

TEST_F(LatencyOptimizerTest, DesktopLatencyTargetIs2Seconds) {
    auto config = LatencyConfig::forDesktop();

    EXPECT_EQ(config.targetGlassToGlassLatency.count(), 2000);
    EXPECT_EQ(config.maxProcessingTime.count(), 50);
}

TEST_F(LatencyOptimizerTest, MobileLatencyTargetIs3Seconds) {
    auto config = LatencyConfig::forMobile();

    EXPECT_EQ(config.targetGlassToGlassLatency.count(), 3000);
    EXPECT_EQ(config.maxProcessingTime.count(), 50);
}

TEST_F(LatencyOptimizerTest, DefaultConfigUsesCurrentPlatform) {
    auto config = LatencyConfig::forCurrentPlatform();

    if (core::isDesktopPlatform()) {
        EXPECT_EQ(config.targetGlassToGlassLatency.count(), 2000);
    } else if (core::isMobilePlatform()) {
        EXPECT_EQ(config.targetGlassToGlassLatency.count(), 3000);
    }
}

TEST_F(LatencyOptimizerTest, ConfigurationCanBeChanged) {
    LatencyConfig config;
    config.targetGlassToGlassLatency = std::chrono::milliseconds(1500);
    config.maxProcessingTime = std::chrono::milliseconds(30);
    config.lowLatencyMode = true;

    optimizer_->setConfig(config);

    auto retrievedConfig = optimizer_->getConfig();
    EXPECT_EQ(retrievedConfig.targetGlassToGlassLatency.count(), 1500);
    EXPECT_EQ(retrievedConfig.maxProcessingTime.count(), 30);
    EXPECT_TRUE(retrievedConfig.lowLatencyMode);
}

// =============================================================================
// Low-Latency Mode Tests (Requirements 12.4, 12.5)
// =============================================================================

TEST_F(LatencyOptimizerTest, LowLatencyModeHas500msMaxBuffer) {
    auto config = LatencyConfig::lowLatency();

    EXPECT_TRUE(config.lowLatencyMode);
    EXPECT_EQ(config.maxSubscriberBuffer.count(), 500);
}

TEST_F(LatencyOptimizerTest, LowLatencyModeReducesBuffering) {
    LatencyConfig normalConfig;
    normalConfig.lowLatencyMode = false;
    normalConfig.maxSubscriberBuffer = std::chrono::milliseconds(5000);

    LatencyConfig lowLatencyConfig = LatencyConfig::lowLatency();

    EXPECT_LT(lowLatencyConfig.maxSubscriberBuffer.count(),
              normalConfig.maxSubscriberBuffer.count());
}

TEST_F(LatencyOptimizerTest, EnableLowLatencyModeUpdatesBuffer) {
    auto config = optimizer_->getConfig();
    config.lowLatencyMode = false;
    config.maxSubscriberBuffer = std::chrono::milliseconds(5000);
    optimizer_->setConfig(config);

    optimizer_->enableLowLatencyMode();

    auto updatedConfig = optimizer_->getConfig();
    EXPECT_TRUE(updatedConfig.lowLatencyMode);
    EXPECT_EQ(updatedConfig.maxSubscriberBuffer.count(), 500);
}

TEST_F(LatencyOptimizerTest, DisableLowLatencyModeRestoresBuffer) {
    optimizer_->enableLowLatencyMode();

    optimizer_->disableLowLatencyMode();

    auto config = optimizer_->getConfig();
    EXPECT_FALSE(config.lowLatencyMode);
    EXPECT_GT(config.maxSubscriberBuffer.count(), 500);
}

// =============================================================================
// Chunk Processing Time Tests (Requirement 12.3)
// =============================================================================

TEST_F(LatencyOptimizerTest, RecordsChunkReceiveTime) {
    uint32_t chunkId = 1;

    optimizer_->recordChunkReceived(chunkId);

    auto receiveTime = optimizer_->getChunkReceiveTime(chunkId);
    ASSERT_TRUE(receiveTime.has_value());

    // Time should be within the last second
    auto now = std::chrono::steady_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - *receiveTime);
    EXPECT_LT(diff.count(), 1000);
}

TEST_F(LatencyOptimizerTest, RecordsChunkForwardTime) {
    uint32_t chunkId = 1;

    optimizer_->recordChunkReceived(chunkId);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    optimizer_->recordChunkForwarded(chunkId);

    auto processingTime = optimizer_->getChunkProcessingTime(chunkId);
    ASSERT_TRUE(processingTime.has_value());
    EXPECT_GE(processingTime->count(), 10);
}

TEST_F(LatencyOptimizerTest, ProcessingTimeWithin50ms) {
    // Record a chunk that was processed within target
    uint32_t chunkId = 1;
    optimizer_->recordChunkReceived(chunkId);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    optimizer_->recordChunkForwarded(chunkId);

    auto processingTime = optimizer_->getChunkProcessingTime(chunkId);
    ASSERT_TRUE(processingTime.has_value());

    auto config = optimizer_->getConfig();
    EXPECT_LE(processingTime->count(), config.maxProcessingTime.count());
}

TEST_F(LatencyOptimizerTest, DetectsProcessingTimeExceedsTarget) {
    uint32_t chunkId = 1;
    optimizer_->recordChunkReceived(chunkId);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    optimizer_->recordChunkForwarded(chunkId);

    EXPECT_TRUE(optimizer_->wasProcessingTimeSlow(chunkId));
}

TEST_F(LatencyOptimizerTest, CountsSlowProcessedChunks) {
    // Process some chunks fast
    for (uint32_t i = 0; i < 3; ++i) {
        optimizer_->recordChunkReceived(i);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        optimizer_->recordChunkForwarded(i);
    }

    // Process some chunks slowly
    for (uint32_t i = 10; i < 12; ++i) {
        optimizer_->recordChunkReceived(i);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        optimizer_->recordChunkForwarded(i);
    }

    auto stats = optimizer_->getProcessingStatistics();
    EXPECT_GE(stats.slowChunksCount, 2u);
}

// =============================================================================
// Glass-to-Glass Latency Tracking Tests
// =============================================================================

TEST_F(LatencyOptimizerTest, TracksStreamLatency) {
    StreamKey key("live", "test");

    optimizer_->startStreamTracking(key);

    // Simulate some frames with known latencies
    for (int i = 0; i < 10; ++i) {
        optimizer_->recordFrameLatency(key, std::chrono::milliseconds(100 + i * 10));
    }

    auto latency = optimizer_->getStreamLatency(key);
    ASSERT_TRUE(latency.has_value());
    EXPECT_GE(latency->averageLatency.count(), 100);
    EXPECT_LE(latency->averageLatency.count(), 200);
}

TEST_F(LatencyOptimizerTest, ChecksLatencyWithinDesktopTarget) {
    LatencyConfig config = LatencyConfig::forDesktop();
    optimizer_->setConfig(config);

    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Record latencies below target
    for (int i = 0; i < 10; ++i) {
        optimizer_->recordFrameLatency(key, std::chrono::milliseconds(500));
    }

    EXPECT_TRUE(optimizer_->isWithinLatencyTarget(key));
}

TEST_F(LatencyOptimizerTest, DetectsLatencyExceedsDesktopTarget) {
    LatencyConfig config = LatencyConfig::forDesktop();
    optimizer_->setConfig(config);

    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Record latencies above target (2 seconds)
    for (int i = 0; i < 10; ++i) {
        optimizer_->recordFrameLatency(key, std::chrono::milliseconds(2500));
    }

    EXPECT_FALSE(optimizer_->isWithinLatencyTarget(key));
}

TEST_F(LatencyOptimizerTest, ChecksLatencyWithinMobileTarget) {
    LatencyConfig config = LatencyConfig::forMobile();
    optimizer_->setConfig(config);

    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Record latencies between desktop and mobile targets (2.5s)
    for (int i = 0; i < 10; ++i) {
        optimizer_->recordFrameLatency(key, std::chrono::milliseconds(2500));
    }

    // Should be within mobile target (3s) but not desktop (2s)
    EXPECT_TRUE(optimizer_->isWithinLatencyTarget(key));
}

// =============================================================================
// Latency Statistics Tests
// =============================================================================

TEST_F(LatencyOptimizerTest, CalculatesAverageLatency) {
    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    optimizer_->recordFrameLatency(key, std::chrono::milliseconds(100));
    optimizer_->recordFrameLatency(key, std::chrono::milliseconds(200));
    optimizer_->recordFrameLatency(key, std::chrono::milliseconds(300));

    auto stats = optimizer_->getStreamLatency(key);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->averageLatency.count(), 200);
}

TEST_F(LatencyOptimizerTest, TracksMinMaxLatency) {
    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    optimizer_->recordFrameLatency(key, std::chrono::milliseconds(100));
    optimizer_->recordFrameLatency(key, std::chrono::milliseconds(500));
    optimizer_->recordFrameLatency(key, std::chrono::milliseconds(200));

    auto stats = optimizer_->getStreamLatency(key);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->minLatency.count(), 100);
    EXPECT_EQ(stats->maxLatency.count(), 500);
}

TEST_F(LatencyOptimizerTest, TracksLatencyPercentiles) {
    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Record 100 latency samples with increasing values
    for (int i = 1; i <= 100; ++i) {
        optimizer_->recordFrameLatency(key, std::chrono::milliseconds(i * 10));
    }

    auto stats = optimizer_->getStreamLatency(key);
    ASSERT_TRUE(stats.has_value());

    // p50 should be around 500ms
    EXPECT_GE(stats->p50Latency.count(), 450);
    EXPECT_LE(stats->p50Latency.count(), 550);

    // p95 should be around 950ms
    EXPECT_GE(stats->p95Latency.count(), 900);
    EXPECT_LE(stats->p95Latency.count(), 1000);

    // p99 should be around 990ms
    EXPECT_GE(stats->p99Latency.count(), 950);
}

TEST_F(LatencyOptimizerTest, TracksLatencySampleCount) {
    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    for (int i = 0; i < 50; ++i) {
        optimizer_->recordFrameLatency(key, std::chrono::milliseconds(100));
    }

    auto stats = optimizer_->getStreamLatency(key);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->sampleCount, 50u);
}

// =============================================================================
// Processing Statistics Tests
// =============================================================================

TEST_F(LatencyOptimizerTest, InitialProcessingStatsAreZero) {
    auto stats = optimizer_->getProcessingStatistics();

    EXPECT_EQ(stats.totalChunksProcessed, 0u);
    EXPECT_EQ(stats.slowChunksCount, 0u);
    EXPECT_EQ(stats.averageProcessingTime.count(), 0);
}

TEST_F(LatencyOptimizerTest, TracksProcessedChunkCount) {
    for (uint32_t i = 0; i < 5; ++i) {
        optimizer_->recordChunkReceived(i);
        optimizer_->recordChunkForwarded(i);
    }

    auto stats = optimizer_->getProcessingStatistics();
    EXPECT_EQ(stats.totalChunksProcessed, 5u);
}

TEST_F(LatencyOptimizerTest, CalculatesAverageProcessingTime) {
    for (uint32_t i = 0; i < 3; ++i) {
        optimizer_->recordChunkReceived(i);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        optimizer_->recordChunkForwarded(i);
    }

    auto stats = optimizer_->getProcessingStatistics();
    EXPECT_GE(stats.averageProcessingTime.count(), 10);
    EXPECT_LT(stats.averageProcessingTime.count(), 50);
}

// =============================================================================
// Stream Lifecycle Tests
// =============================================================================

TEST_F(LatencyOptimizerTest, StartAndStopStreamTracking) {
    StreamKey key("live", "test");

    optimizer_->startStreamTracking(key);
    EXPECT_TRUE(optimizer_->isTrackingStream(key));

    optimizer_->stopStreamTracking(key);
    EXPECT_FALSE(optimizer_->isTrackingStream(key));
}

TEST_F(LatencyOptimizerTest, GetAllTrackedStreams) {
    StreamKey key1("live", "test1");
    StreamKey key2("live", "test2");

    optimizer_->startStreamTracking(key1);
    optimizer_->startStreamTracking(key2);

    auto streams = optimizer_->getTrackedStreams();
    EXPECT_EQ(streams.size(), 2u);
}

TEST_F(LatencyOptimizerTest, StopTrackingClearsData) {
    StreamKey key("live", "test");

    optimizer_->startStreamTracking(key);
    optimizer_->recordFrameLatency(key, std::chrono::milliseconds(100));

    optimizer_->stopStreamTracking(key);

    auto stats = optimizer_->getStreamLatency(key);
    EXPECT_FALSE(stats.has_value());
}

// =============================================================================
// Latency Alerts and Callbacks Tests
// =============================================================================

TEST_F(LatencyOptimizerTest, CallbackInvokedOnHighLatency) {
    bool callbackInvoked = false;
    StreamKey capturedKey;
    std::chrono::milliseconds capturedLatency{0};

    optimizer_->setOnHighLatencyCallback([&](const StreamKey& key,
                                              std::chrono::milliseconds latency) {
        callbackInvoked = true;
        capturedKey = key;
        capturedLatency = latency;
    });

    LatencyConfig config = LatencyConfig::forDesktop();
    config.alertThreshold = std::chrono::milliseconds(1000);
    optimizer_->setConfig(config);

    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Record latency above threshold
    optimizer_->recordFrameLatency(key, std::chrono::milliseconds(1500));

    EXPECT_TRUE(callbackInvoked);
    EXPECT_EQ(capturedKey.name, "test");
    EXPECT_EQ(capturedLatency.count(), 1500);
}

TEST_F(LatencyOptimizerTest, CallbackInvokedOnSlowProcessing) {
    bool callbackInvoked = false;
    uint32_t capturedChunkId = 0;

    optimizer_->setOnSlowProcessingCallback([&](uint32_t chunkId,
                                                 std::chrono::milliseconds time) {
        callbackInvoked = true;
        capturedChunkId = chunkId;
    });

    uint32_t chunkId = 42;
    optimizer_->recordChunkReceived(chunkId);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    optimizer_->recordChunkForwarded(chunkId);

    EXPECT_TRUE(callbackInvoked);
    EXPECT_EQ(capturedChunkId, 42u);
}

// =============================================================================
// Buffer Size Recommendation Tests
// =============================================================================

TEST_F(LatencyOptimizerTest, RecommendedBufferSizeInNormalMode) {
    LatencyConfig config;
    config.lowLatencyMode = false;
    optimizer_->setConfig(config);

    auto recommendedSize = optimizer_->getRecommendedBufferSize();
    EXPECT_GT(recommendedSize.count(), 500);  // Greater than low-latency max
}

TEST_F(LatencyOptimizerTest, RecommendedBufferSizeInLowLatencyMode) {
    optimizer_->enableLowLatencyMode();

    auto recommendedSize = optimizer_->getRecommendedBufferSize();
    EXPECT_LE(recommendedSize.count(), 500);
}

// =============================================================================
// Reset and Clear Tests
// =============================================================================

TEST_F(LatencyOptimizerTest, ResetClearsAllStatistics) {
    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);
    optimizer_->recordFrameLatency(key, std::chrono::milliseconds(100));

    for (uint32_t i = 0; i < 5; ++i) {
        optimizer_->recordChunkReceived(i);
        optimizer_->recordChunkForwarded(i);
    }

    optimizer_->reset();

    auto procStats = optimizer_->getProcessingStatistics();
    EXPECT_EQ(procStats.totalChunksProcessed, 0u);

    // Stream tracking should also be cleared
    EXPECT_FALSE(optimizer_->isTrackingStream(key));
}

TEST_F(LatencyOptimizerTest, ResetPreservesConfiguration) {
    LatencyConfig config = LatencyConfig::lowLatency();
    optimizer_->setConfig(config);

    optimizer_->reset();

    auto retrievedConfig = optimizer_->getConfig();
    EXPECT_TRUE(retrievedConfig.lowLatencyMode);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

class LatencyOptimizerConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        optimizer_ = std::make_unique<LatencyOptimizer>();
    }

    std::unique_ptr<LatencyOptimizer> optimizer_;
};

TEST_F(LatencyOptimizerConcurrencyTest, ConcurrentChunkRecordingIsThreadSafe) {
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;
    std::atomic<uint32_t> chunkCounter{0};

    // Multiple producer threads recording chunks
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([this, &running, &chunkCounter]() {
            while (running) {
                uint32_t chunkId = chunkCounter.fetch_add(1);
                optimizer_->recordChunkReceived(chunkId);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                optimizer_->recordChunkForwarded(chunkId);
            }
        });
    }

    // Run for a short time
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& thread : threads) {
        thread.join();
    }

    // Test passes if no crashes or deadlocks
    auto stats = optimizer_->getProcessingStatistics();
    EXPECT_GT(stats.totalChunksProcessed, 0u);
}

TEST_F(LatencyOptimizerConcurrencyTest, ConcurrentLatencyRecordingIsThreadSafe) {
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;

    StreamKey key("live", "test");
    optimizer_->startStreamTracking(key);

    // Multiple producer threads recording latency
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([this, &running, &key]() {
            while (running) {
                optimizer_->recordFrameLatency(key, std::chrono::milliseconds(100));
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }

    // Reader thread
    threads.emplace_back([this, &running, &key]() {
        while (running) {
            optimizer_->getStreamLatency(key);
            optimizer_->isWithinLatencyTarget(key);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& thread : threads) {
        thread.join();
    }

    // Test passes if no crashes or deadlocks
    auto stats = optimizer_->getStreamLatency(key);
    ASSERT_TRUE(stats.has_value());
    EXPECT_GT(stats->sampleCount, 0u);
}

// =============================================================================
// Integration Tests
// =============================================================================

TEST_F(LatencyOptimizerTest, EndToEndLatencyTracking) {
    // Configure for desktop targets
    LatencyConfig config = LatencyConfig::forDesktop();
    optimizer_->setConfig(config);

    StreamKey key("live", "integration_test");
    optimizer_->startStreamTracking(key);

    // Simulate stream processing
    for (int frame = 0; frame < 100; ++frame) {
        uint32_t chunkId = static_cast<uint32_t>(frame);

        // Record chunk processing
        optimizer_->recordChunkReceived(chunkId);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        optimizer_->recordChunkForwarded(chunkId);

        // Record end-to-end latency
        optimizer_->recordFrameLatency(key, std::chrono::milliseconds(500 + (frame % 100)));
    }

    // Verify processing statistics
    auto procStats = optimizer_->getProcessingStatistics();
    EXPECT_EQ(procStats.totalChunksProcessed, 100u);
    EXPECT_EQ(procStats.slowChunksCount, 0u);  // All should be within 50ms

    // Verify latency statistics
    auto latencyStats = optimizer_->getStreamLatency(key);
    ASSERT_TRUE(latencyStats.has_value());
    EXPECT_EQ(latencyStats->sampleCount, 100u);
    EXPECT_GE(latencyStats->minLatency.count(), 500);
    EXPECT_LT(latencyStats->maxLatency.count(), 600);

    // Should be within desktop target (2 seconds)
    EXPECT_TRUE(optimizer_->isWithinLatencyTarget(key));

    optimizer_->stopStreamTracking(key);
}

} // namespace test
} // namespace streaming
} // namespace openrtmp
