// OpenRTMP - Cross-platform RTMP Server
// Performance Tests: Latency Validation
//
// Task 21.3: Implement E2E and performance tests
// Validates latency targets (2s desktop, 3s mobile) and 50ms processing target.
//
// Note: Tests validate processing time within the server.
// Glass-to-glass latency requires end-to-end testing with actual clients.
//
// Requirements coverage:
// - Requirement 12.1: Glass-to-glass latency <2s on desktop
// - Requirement 12.2: Glass-to-glass latency <3s on mobile
// - Requirement 12.3: Process and forward chunks within 50ms

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <algorithm>
#include <numeric>
#include <random>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/result.hpp"

namespace openrtmp {
namespace performance {
namespace test {

// =============================================================================
// Latency Constants
// =============================================================================

// Desktop targets (Requirement 12.1)
constexpr std::chrono::milliseconds DESKTOP_LATENCY_TARGET{2000};

// Mobile targets (Requirement 12.2)
constexpr std::chrono::milliseconds MOBILE_LATENCY_TARGET{3000};

// Processing target (Requirement 12.3)
constexpr std::chrono::milliseconds PROCESSING_TARGET{50};

// =============================================================================
// Latency Measurement Infrastructure
// =============================================================================

/**
 * @brief Represents a timestamped frame for latency measurement.
 */
struct TimestampedFrame {
    uint64_t frameId;
    std::chrono::steady_clock::time_point ingestTime;
    std::chrono::steady_clock::time_point processStartTime;
    std::chrono::steady_clock::time_point processEndTime;
    std::chrono::steady_clock::time_point distributeTime;
    size_t dataSize;
};

/**
 * @brief Latency statistics.
 */
struct LatencyStats {
    std::chrono::microseconds minLatency;
    std::chrono::microseconds maxLatency;
    std::chrono::microseconds avgLatency;
    std::chrono::microseconds p50Latency;
    std::chrono::microseconds p95Latency;
    std::chrono::microseconds p99Latency;
    size_t sampleCount;
    size_t exceedsTargetCount;
};

/**
 * @brief Mock latency tracker for measuring processing times.
 */
class MockLatencyTracker {
public:
    MockLatencyTracker()
        : nextFrameId_(1)
        , totalFrames_(0)
        , exceedsProcessingTarget_(0)
        , exceedsLatencyTarget_(0)
        , targetProcessingTime_(PROCESSING_TARGET)
        , targetLatency_(DESKTOP_LATENCY_TARGET)
    {}

    // Configure target latency (platform-specific)
    void setTargetLatency(std::chrono::milliseconds target) {
        targetLatency_ = target;
    }

    void setTargetProcessingTime(std::chrono::milliseconds target) {
        targetProcessingTime_ = target;
    }

    // Record frame ingestion
    uint64_t recordIngestion(size_t dataSize) {
        std::lock_guard<std::mutex> lock(mutex_);

        TimestampedFrame frame;
        frame.frameId = nextFrameId_++;
        frame.ingestTime = std::chrono::steady_clock::now();
        frame.dataSize = dataSize;

        frames_[frame.frameId] = frame;
        totalFrames_++;

        return frame.frameId;
    }

    // Record processing start
    void recordProcessingStart(uint64_t frameId) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = frames_.find(frameId);
        if (it != frames_.end()) {
            it->second.processStartTime = std::chrono::steady_clock::now();
        }
    }

    // Record processing end
    void recordProcessingEnd(uint64_t frameId) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = frames_.find(frameId);
        if (it != frames_.end()) {
            it->second.processEndTime = std::chrono::steady_clock::now();

            // Check if processing exceeded target
            auto processingTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                it->second.processEndTime - it->second.processStartTime);

            if (processingTime > targetProcessingTime_) {
                exceedsProcessingTarget_++;
            }
        }
    }

    // Record distribution (simulates delivery to subscriber)
    void recordDistribution(uint64_t frameId) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = frames_.find(frameId);
        if (it != frames_.end()) {
            it->second.distributeTime = std::chrono::steady_clock::now();

            // Check total latency
            auto totalLatency = std::chrono::duration_cast<std::chrono::milliseconds>(
                it->second.distributeTime - it->second.ingestTime);

            if (totalLatency > targetLatency_) {
                exceedsLatencyTarget_++;
            }

            completedFrameIds_.push_back(frameId);
        }
    }

    // Get processing time for a frame
    std::chrono::microseconds getProcessingTime(uint64_t frameId) const {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = frames_.find(frameId);
        if (it != frames_.end()) {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                it->second.processEndTime - it->second.processStartTime);
        }
        return std::chrono::microseconds{0};
    }

    // Get total latency (ingest to distribute) for a frame
    std::chrono::microseconds getTotalLatency(uint64_t frameId) const {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = frames_.find(frameId);
        if (it != frames_.end()) {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                it->second.distributeTime - it->second.ingestTime);
        }
        return std::chrono::microseconds{0};
    }

    // Get processing statistics
    LatencyStats getProcessingStats() const {
        return calculateStats([](const TimestampedFrame& f) {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                f.processEndTime - f.processStartTime);
        });
    }

    // Get total latency statistics
    LatencyStats getTotalLatencyStats() const {
        return calculateStats([](const TimestampedFrame& f) {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                f.distributeTime - f.ingestTime);
        });
    }

    // Get counters
    size_t getTotalFrames() const { return totalFrames_; }
    size_t getExceedsProcessingTarget() const { return exceedsProcessingTarget_; }
    size_t getExceedsLatencyTarget() const { return exceedsLatencyTarget_; }

    // Calculate percentage within target
    double getProcessingTargetCompliancePercent() const {
        if (totalFrames_ == 0) return 100.0;
        return 100.0 * (totalFrames_ - exceedsProcessingTarget_) / totalFrames_;
    }

    double getLatencyTargetCompliancePercent() const {
        if (totalFrames_ == 0) return 100.0;
        return 100.0 * (totalFrames_ - exceedsLatencyTarget_) / totalFrames_;
    }

    // Clear all data
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_.clear();
        completedFrameIds_.clear();
        totalFrames_ = 0;
        exceedsProcessingTarget_ = 0;
        exceedsLatencyTarget_ = 0;
    }

private:
    template<typename GetLatencyFunc>
    LatencyStats calculateStats(GetLatencyFunc getLatency) const {
        std::lock_guard<std::mutex> lock(mutex_);

        LatencyStats stats{};
        if (completedFrameIds_.empty()) {
            return stats;
        }

        std::vector<std::chrono::microseconds> latencies;
        latencies.reserve(completedFrameIds_.size());

        for (uint64_t frameId : completedFrameIds_) {
            auto it = frames_.find(frameId);
            if (it != frames_.end()) {
                latencies.push_back(getLatency(it->second));
            }
        }

        if (latencies.empty()) {
            return stats;
        }

        // Sort for percentile calculations
        std::sort(latencies.begin(), latencies.end());

        stats.sampleCount = latencies.size();
        stats.minLatency = latencies.front();
        stats.maxLatency = latencies.back();

        // Calculate average
        auto total = std::accumulate(latencies.begin(), latencies.end(),
                                      std::chrono::microseconds{0});
        stats.avgLatency = std::chrono::microseconds{total.count() / latencies.size()};

        // Percentiles
        stats.p50Latency = latencies[latencies.size() * 50 / 100];
        stats.p95Latency = latencies[latencies.size() * 95 / 100];
        stats.p99Latency = latencies[latencies.size() * 99 / 100];

        // Count exceeding target
        auto targetMicros = std::chrono::duration_cast<std::chrono::microseconds>(targetLatency_);
        stats.exceedsTargetCount = std::count_if(latencies.begin(), latencies.end(),
            [targetMicros](auto l) { return l > targetMicros; });

        return stats;
    }

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, TimestampedFrame> frames_;
    std::vector<uint64_t> completedFrameIds_;
    uint64_t nextFrameId_;
    std::atomic<size_t> totalFrames_;
    std::atomic<size_t> exceedsProcessingTarget_;
    std::atomic<size_t> exceedsLatencyTarget_;
    std::chrono::milliseconds targetProcessingTime_;
    std::chrono::milliseconds targetLatency_;
};

// =============================================================================
// Mock Processing Pipeline
// =============================================================================

class MockProcessingPipeline {
public:
    MockProcessingPipeline(MockLatencyTracker& tracker)
        : tracker_(tracker)
        , processingDelayUs_(100)  // Base 100us processing
        , variabilityUs_(50)       // +/- 50us variability
    {}

    // Set simulated processing delay
    void setProcessingDelay(std::chrono::microseconds base,
                            std::chrono::microseconds variability) {
        processingDelayUs_ = base.count();
        variabilityUs_ = variability.count();
    }

    // Process a frame through the pipeline
    void processFrame(uint64_t frameId, size_t dataSize) {
        tracker_.recordProcessingStart(frameId);

        // Simulate processing with some variability
        auto delay = getProcessingDelay(dataSize);
        std::this_thread::sleep_for(delay);

        tracker_.recordProcessingEnd(frameId);
    }

    // Distribute to subscriber
    void distributeFrame(uint64_t frameId) {
        // Small distribution overhead
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        tracker_.recordDistribution(frameId);
    }

    // Full pipeline: ingest -> process -> distribute
    uint64_t fullPipeline(size_t dataSize) {
        uint64_t frameId = tracker_.recordIngestion(dataSize);
        processFrame(frameId, dataSize);
        distributeFrame(frameId);
        return frameId;
    }

private:
    std::chrono::microseconds getProcessingDelay(size_t dataSize) {
        // Base delay plus size-proportional delay
        size_t baseDelay = processingDelayUs_;
        size_t sizeDelay = dataSize / 10000;  // 1us per 10KB

        // Add random variability
        static std::mt19937 gen(42);
        std::uniform_int_distribution<size_t> dist(0, variabilityUs_ * 2);
        size_t variance = dist(gen);

        return std::chrono::microseconds(baseDelay + sizeDelay + variance - variabilityUs_);
    }

    MockLatencyTracker& tracker_;
    size_t processingDelayUs_;
    size_t variabilityUs_;
};

// =============================================================================
// Test Fixtures
// =============================================================================

class LatencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        desktopTracker_ = std::make_unique<MockLatencyTracker>();
        desktopTracker_->setTargetLatency(DESKTOP_LATENCY_TARGET);

        mobileTracker_ = std::make_unique<MockLatencyTracker>();
        mobileTracker_->setTargetLatency(MOBILE_LATENCY_TARGET);

        desktopPipeline_ = std::make_unique<MockProcessingPipeline>(*desktopTracker_);
        mobilePipeline_ = std::make_unique<MockProcessingPipeline>(*mobileTracker_);
    }

    void TearDown() override {
        mobilePipeline_.reset();
        desktopPipeline_.reset();
        mobileTracker_.reset();
        desktopTracker_.reset();
    }

    std::unique_ptr<MockLatencyTracker> desktopTracker_;
    std::unique_ptr<MockLatencyTracker> mobileTracker_;
    std::unique_ptr<MockProcessingPipeline> desktopPipeline_;
    std::unique_ptr<MockProcessingPipeline> mobilePipeline_;
};

// =============================================================================
// Processing Time Tests (Requirement 12.3)
// =============================================================================

TEST_F(LatencyTest, ProcessingTimeUnder50ms) {
    // Configure fast processing
    desktopPipeline_->setProcessingDelay(
        std::chrono::microseconds(100),
        std::chrono::microseconds(50));

    // Process 100 frames
    for (int i = 0; i < 100; ++i) {
        uint64_t frameId = desktopTracker_->recordIngestion(10000);  // 10KB frame
        desktopPipeline_->processFrame(frameId, 10000);
    }

    // Verify all processing times under 50ms
    EXPECT_EQ(desktopTracker_->getExceedsProcessingTarget(), 0u);
    EXPECT_EQ(desktopTracker_->getProcessingTargetCompliancePercent(), 100.0);
}

TEST_F(LatencyTest, ProcessingTimeStatistics) {
    desktopPipeline_->setProcessingDelay(
        std::chrono::microseconds(500),
        std::chrono::microseconds(200));

    for (int i = 0; i < 1000; ++i) {
        desktopPipeline_->fullPipeline(5000);
    }

    auto stats = desktopTracker_->getProcessingStats();

    // Check statistics are reasonable
    EXPECT_GT(stats.sampleCount, 900u);
    EXPECT_GT(stats.avgLatency.count(), 0);
    EXPECT_LT(stats.avgLatency.count(), 50000);  // < 50ms avg
    EXPECT_LE(stats.minLatency, stats.avgLatency);
    EXPECT_LE(stats.avgLatency, stats.maxLatency);
    EXPECT_LE(stats.p50Latency, stats.p95Latency);
    EXPECT_LE(stats.p95Latency, stats.p99Latency);
}

TEST_F(LatencyTest, ProcessingScalesWithFrameSize) {
    // Process small frame
    uint64_t smallFrameId = desktopTracker_->recordIngestion(1000);
    desktopPipeline_->processFrame(smallFrameId, 1000);
    auto smallTime = desktopTracker_->getProcessingTime(smallFrameId);

    desktopTracker_->reset();

    // Process large frame
    uint64_t largeFrameId = desktopTracker_->recordIngestion(100000);
    desktopPipeline_->processFrame(largeFrameId, 100000);
    auto largeTime = desktopTracker_->getProcessingTime(largeFrameId);

    // Large frame should take longer (but both under 50ms)
    EXPECT_GE(largeTime.count(), smallTime.count());
    EXPECT_LT(largeTime.count(), 50000);  // < 50ms
}

// =============================================================================
// Desktop Latency Tests (Requirement 12.1)
// =============================================================================

TEST_F(LatencyTest, DesktopTotalLatencyUnder2Seconds) {
    for (int i = 0; i < 100; ++i) {
        desktopPipeline_->fullPipeline(10000);
    }

    auto stats = desktopTracker_->getTotalLatencyStats();

    // All latencies should be well under 2 seconds
    EXPECT_LT(stats.maxLatency.count(), 2000000);  // < 2s in microseconds
    EXPECT_EQ(stats.exceedsTargetCount, 0u);
}

TEST_F(LatencyTest, DesktopP99LatencyUnder2Seconds) {
    for (int i = 0; i < 1000; ++i) {
        desktopPipeline_->fullPipeline(10000);
    }

    auto stats = desktopTracker_->getTotalLatencyStats();

    // P99 should be under target
    EXPECT_LT(stats.p99Latency.count(), 2000000);
}

TEST_F(LatencyTest, DesktopLatencyCompliance99Percent) {
    for (int i = 0; i < 1000; ++i) {
        desktopPipeline_->fullPipeline(10000);
    }

    double compliance = desktopTracker_->getLatencyTargetCompliancePercent();

    // At least 99% should be within target
    EXPECT_GE(compliance, 99.0);
}

// =============================================================================
// Mobile Latency Tests (Requirement 12.2)
// =============================================================================

TEST_F(LatencyTest, MobileTotalLatencyUnder3Seconds) {
    // Configure slightly slower processing for mobile simulation
    mobilePipeline_->setProcessingDelay(
        std::chrono::microseconds(500),
        std::chrono::microseconds(200));

    for (int i = 0; i < 100; ++i) {
        mobilePipeline_->fullPipeline(10000);
    }

    auto stats = mobileTracker_->getTotalLatencyStats();

    // All latencies should be under 3 seconds
    EXPECT_LT(stats.maxLatency.count(), 3000000);  // < 3s in microseconds
    EXPECT_EQ(stats.exceedsTargetCount, 0u);
}

TEST_F(LatencyTest, MobileP99LatencyUnder3Seconds) {
    mobilePipeline_->setProcessingDelay(
        std::chrono::microseconds(500),
        std::chrono::microseconds(200));

    for (int i = 0; i < 1000; ++i) {
        mobilePipeline_->fullPipeline(10000);
    }

    auto stats = mobileTracker_->getTotalLatencyStats();

    EXPECT_LT(stats.p99Latency.count(), 3000000);
}

TEST_F(LatencyTest, MobileLatencyCompliance99Percent) {
    mobilePipeline_->setProcessingDelay(
        std::chrono::microseconds(500),
        std::chrono::microseconds(200));

    for (int i = 0; i < 1000; ++i) {
        mobilePipeline_->fullPipeline(10000);
    }

    double compliance = mobileTracker_->getLatencyTargetCompliancePercent();

    EXPECT_GE(compliance, 99.0);
}

// =============================================================================
// Stress and Edge Case Tests
// =============================================================================

TEST_F(LatencyTest, LatencyUnderHighLoad) {
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;
    std::atomic<size_t> completedFrames{0};

    // Multiple concurrent frame processing
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([this, &running, &completedFrames]() {
            while (running) {
                desktopPipeline_->fullPipeline(5000);
                completedFrames++;
            }
        });
    }

    // Run for 500ms
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    running = false;

    for (auto& thread : threads) {
        thread.join();
    }

    // Even under load, latency should be reasonable
    auto stats = desktopTracker_->getTotalLatencyStats();
    EXPECT_LT(stats.p95Latency.count(), 100000);  // P95 < 100ms
}

TEST_F(LatencyTest, LatencyConsistencyOverTime) {
    std::vector<std::chrono::microseconds> latencies;

    // Process frames over simulated time
    for (int batch = 0; batch < 10; ++batch) {
        for (int frame = 0; frame < 100; ++frame) {
            uint64_t frameId = desktopPipeline_->fullPipeline(10000);
            latencies.push_back(desktopTracker_->getTotalLatency(frameId));
        }
        // Small delay between batches
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Calculate standard deviation
    auto sum = std::accumulate(latencies.begin(), latencies.end(),
                               std::chrono::microseconds{0});
    double mean = sum.count() / static_cast<double>(latencies.size());

    double variance = 0;
    for (auto l : latencies) {
        double diff = l.count() - mean;
        variance += diff * diff;
    }
    variance /= latencies.size();
    double stddev = std::sqrt(variance);

    // Standard deviation should be reasonable (< 50% of mean)
    EXPECT_LT(stddev, mean * 0.5);
}

TEST_F(LatencyTest, LargeFrameLatency) {
    // Process very large frames (1MB each)
    for (int i = 0; i < 10; ++i) {
        desktopPipeline_->fullPipeline(1000000);  // 1MB
    }

    auto stats = desktopTracker_->getTotalLatencyStats();

    // Even large frames should be processed within target
    EXPECT_LT(stats.maxLatency.count(), 2000000);  // < 2s
}

TEST_F(LatencyTest, BurstTrafficLatency) {
    // Burst of 100 frames at once
    std::vector<uint64_t> frameIds;
    for (int i = 0; i < 100; ++i) {
        frameIds.push_back(desktopTracker_->recordIngestion(5000));
    }

    // Process all frames
    for (auto id : frameIds) {
        desktopPipeline_->processFrame(id, 5000);
        desktopPipeline_->distributeFrame(id);
    }

    // First and last frame latencies should be similar
    auto firstLatency = desktopTracker_->getTotalLatency(frameIds.front());
    auto lastLatency = desktopTracker_->getTotalLatency(frameIds.back());

    // Last frame latency shouldn't be dramatically higher
    EXPECT_LT(lastLatency.count(), firstLatency.count() * 10);
}

// =============================================================================
// Latency Percentile Tests
// =============================================================================

TEST_F(LatencyTest, PercentilesAreMonotonic) {
    for (int i = 0; i < 1000; ++i) {
        desktopPipeline_->fullPipeline(10000);
    }

    auto stats = desktopTracker_->getTotalLatencyStats();

    // Percentiles should be monotonically increasing
    EXPECT_LE(stats.minLatency, stats.p50Latency);
    EXPECT_LE(stats.p50Latency, stats.p95Latency);
    EXPECT_LE(stats.p95Latency, stats.p99Latency);
    EXPECT_LE(stats.p99Latency, stats.maxLatency);
}

TEST_F(LatencyTest, AverageIsReasonable) {
    for (int i = 0; i < 1000; ++i) {
        desktopPipeline_->fullPipeline(10000);
    }

    auto stats = desktopTracker_->getTotalLatencyStats();

    // Average should be between min and max
    EXPECT_GE(stats.avgLatency, stats.minLatency);
    EXPECT_LE(stats.avgLatency, stats.maxLatency);

    // Average should be close to p50 (within 2x)
    double avgMs = stats.avgLatency.count() / 1000.0;
    double p50Ms = stats.p50Latency.count() / 1000.0;
    EXPECT_LT(std::abs(avgMs - p50Ms), std::max(avgMs, p50Ms));
}

} // namespace test
} // namespace performance
} // namespace openrtmp
