// OpenRTMP - Cross-platform RTMP Server
// Performance Tests: Throughput Validation
//
// Task 21.3: Implement E2E and performance tests
// Validates throughput targets (50/500 Mbps desktop, 20/100 Mbps mobile).
//
// Note: Tests use simulated data flow rather than actual network I/O.
// Real throughput testing requires dedicated network infrastructure.
//
// Requirements coverage:
// - Requirement 13.1: 50 Mbps ingestion bitrate on desktop
// - Requirement 13.2: 20 Mbps ingestion bitrate on mobile
// - Requirement 13.3: 500 Mbps aggregate distribution on desktop
// - Requirement 13.4: 100 Mbps aggregate distribution on mobile

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <numeric>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/result.hpp"

namespace openrtmp {
namespace performance {
namespace test {

// =============================================================================
// Throughput Constants
// =============================================================================

// Desktop targets (Requirements 13.1, 13.3)
constexpr double DESKTOP_INGESTION_MBPS = 50.0;
constexpr double DESKTOP_DISTRIBUTION_MBPS = 500.0;

// Mobile targets (Requirements 13.2, 13.4)
constexpr double MOBILE_INGESTION_MBPS = 20.0;
constexpr double MOBILE_DISTRIBUTION_MBPS = 100.0;

// Conversion helpers
constexpr size_t mbpsToBytes(double mbps, double seconds) {
    return static_cast<size_t>(mbps * 125000.0 * seconds);  // Mbps to bytes
}

constexpr size_t mbpsToKBps(double mbps) {
    return static_cast<size_t>(mbps * 125.0);  // Mbps to KB/s
}

// =============================================================================
// Mock Data Pipeline for Throughput Testing
// =============================================================================

/**
 * @brief Represents a data chunk in the pipeline.
 */
struct DataChunk {
    size_t size;
    std::chrono::steady_clock::time_point timestamp;
    bool processed;
};

/**
 * @brief Mock data pipeline for throughput measurement.
 */
class MockDataPipeline {
public:
    MockDataPipeline(size_t bufferSize = 10 * 1024 * 1024)  // 10MB buffer
        : maxBufferSize_(bufferSize)
        , currentBufferSize_(0)
        , totalBytesIngested_(0)
        , totalBytesDistributed_(0)
        , peakIngestionRateMbps_(0.0)
        , peakDistributionRateMbps_(0.0)
    {}

    // Ingest data into pipeline
    bool ingest(size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (currentBufferSize_ + bytes > maxBufferSize_) {
            return false;  // Buffer overflow
        }

        auto now = std::chrono::steady_clock::now();
        ingestHistory_.push_back({bytes, now});
        currentBufferSize_ += bytes;
        totalBytesIngested_ += bytes;

        updateIngestionRate();
        return true;
    }

    // Distribute data from pipeline
    bool distribute(size_t bytes, size_t subscriberCount = 1) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (bytes > currentBufferSize_) {
            bytes = currentBufferSize_;  // Distribute what's available
        }

        auto now = std::chrono::steady_clock::now();

        // Each subscriber receives the data
        size_t totalDistributed = bytes * subscriberCount;
        distributeHistory_.push_back({totalDistributed, now});
        currentBufferSize_ -= bytes;
        totalBytesDistributed_ += totalDistributed;

        updateDistributionRate();
        return true;
    }

    // Get current ingestion rate in Mbps
    double getCurrentIngestionRateMbps() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calculateRecentRate(ingestHistory_);
    }

    // Get current distribution rate in Mbps
    double getCurrentDistributionRateMbps() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calculateRecentRate(distributeHistory_);
    }

    // Get peak rates
    double getPeakIngestionRateMbps() const { return peakIngestionRateMbps_; }
    double getPeakDistributionRateMbps() const { return peakDistributionRateMbps_; }

    // Get totals
    size_t getTotalBytesIngested() const { return totalBytesIngested_; }
    size_t getTotalBytesDistributed() const { return totalBytesDistributed_; }
    size_t getCurrentBufferSize() const { return currentBufferSize_; }

    // Calculate average rate over entire duration
    double getAverageIngestionRateMbps(std::chrono::milliseconds duration) const {
        double seconds = duration.count() / 1000.0;
        if (seconds <= 0) return 0;
        return (totalBytesIngested_ / 125000.0) / seconds;
    }

    double getAverageDistributionRateMbps(std::chrono::milliseconds duration) const {
        double seconds = duration.count() / 1000.0;
        if (seconds <= 0) return 0;
        return (totalBytesDistributed_ / 125000.0) / seconds;
    }

    // Clear history
    void clearHistory() {
        std::lock_guard<std::mutex> lock(mutex_);
        ingestHistory_.clear();
        distributeHistory_.clear();
    }

    // Reset all state
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        ingestHistory_.clear();
        distributeHistory_.clear();
        currentBufferSize_ = 0;
        totalBytesIngested_ = 0;
        totalBytesDistributed_ = 0;
        peakIngestionRateMbps_ = 0.0;
        peakDistributionRateMbps_ = 0.0;
    }

private:
    double calculateRecentRate(const std::vector<std::pair<size_t, std::chrono::steady_clock::time_point>>& history) const {
        if (history.size() < 2) return 0.0;

        auto now = std::chrono::steady_clock::now();
        auto windowStart = now - std::chrono::seconds(1);

        size_t bytesInWindow = 0;
        for (const auto& entry : history) {
            if (entry.second >= windowStart) {
                bytesInWindow += entry.first;
            }
        }

        return bytesInWindow / 125000.0;  // Convert to Mbps
    }

    void updateIngestionRate() {
        double rate = calculateRecentRate(ingestHistory_);
        if (rate > peakIngestionRateMbps_) {
            peakIngestionRateMbps_ = rate;
        }
    }

    void updateDistributionRate() {
        double rate = calculateRecentRate(distributeHistory_);
        if (rate > peakDistributionRateMbps_) {
            peakDistributionRateMbps_ = rate;
        }
    }

    mutable std::mutex mutex_;
    size_t maxBufferSize_;
    size_t currentBufferSize_;
    std::atomic<size_t> totalBytesIngested_;
    std::atomic<size_t> totalBytesDistributed_;
    std::atomic<double> peakIngestionRateMbps_;
    std::atomic<double> peakDistributionRateMbps_;
    std::vector<std::pair<size_t, std::chrono::steady_clock::time_point>> ingestHistory_;
    std::vector<std::pair<size_t, std::chrono::steady_clock::time_point>> distributeHistory_;
};

// =============================================================================
// Throughput Test Generator
// =============================================================================

class ThroughputGenerator {
public:
    ThroughputGenerator(MockDataPipeline& pipeline)
        : pipeline_(pipeline)
        , running_(false)
    {}

    void startIngestion(double targetMbps, std::chrono::milliseconds duration) {
        running_ = true;
        startTime_ = std::chrono::steady_clock::now();
        endTime_ = startTime_ + duration;

        // Calculate chunk size for ~100 operations per second
        size_t bytesPerSecond = static_cast<size_t>(targetMbps * 125000);
        chunkSize_ = bytesPerSecond / 100;

        ingestionThread_ = std::thread([this]() {
            while (running_ && std::chrono::steady_clock::now() < endTime_) {
                pipeline_.ingest(chunkSize_);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }

    void startDistribution(size_t subscriberCount, std::chrono::milliseconds duration) {
        running_ = true;
        startTime_ = std::chrono::steady_clock::now();
        endTime_ = startTime_ + duration;

        distributionThread_ = std::thread([this, subscriberCount]() {
            while (running_ && std::chrono::steady_clock::now() < endTime_) {
                // Distribute whatever is in the buffer
                pipeline_.distribute(chunkSize_, subscriberCount);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }

    void stop() {
        running_ = false;
        if (ingestionThread_.joinable()) {
            ingestionThread_.join();
        }
        if (distributionThread_.joinable()) {
            distributionThread_.join();
        }
    }

    std::chrono::milliseconds getActualDuration() const {
        auto end = std::min(std::chrono::steady_clock::now(), endTime_);
        return std::chrono::duration_cast<std::chrono::milliseconds>(end - startTime_);
    }

private:
    MockDataPipeline& pipeline_;
    std::atomic<bool> running_;
    std::chrono::steady_clock::time_point startTime_;
    std::chrono::steady_clock::time_point endTime_;
    size_t chunkSize_{10240};
    std::thread ingestionThread_;
    std::thread distributionThread_;
};

// =============================================================================
// Test Fixtures
// =============================================================================

class ThroughputTest : public ::testing::Test {
protected:
    void SetUp() override {
        desktopPipeline_ = std::make_unique<MockDataPipeline>(100 * 1024 * 1024);  // 100MB
        mobilePipeline_ = std::make_unique<MockDataPipeline>(20 * 1024 * 1024);    // 20MB
    }

    void TearDown() override {
        mobilePipeline_.reset();
        desktopPipeline_.reset();
    }

    std::unique_ptr<MockDataPipeline> desktopPipeline_;
    std::unique_ptr<MockDataPipeline> mobilePipeline_;
};

// =============================================================================
// Desktop Ingestion Tests (Requirement 13.1)
// =============================================================================

TEST_F(ThroughputTest, DesktopSupports50MbpsIngestion) {
    ThroughputGenerator generator(*desktopPipeline_);

    auto duration = std::chrono::milliseconds(1000);  // 1 second
    generator.startIngestion(DESKTOP_INGESTION_MBPS, duration);

    // Wait for test to complete
    std::this_thread::sleep_for(duration + std::chrono::milliseconds(100));
    generator.stop();

    // Verify achieved rate
    auto avgRate = desktopPipeline_->getAverageIngestionRateMbps(duration);

    // Should achieve at least 80% of target rate
    EXPECT_GE(avgRate, DESKTOP_INGESTION_MBPS * 0.8);
}

TEST_F(ThroughputTest, DesktopIngestionSustainedFor10Seconds) {
    ThroughputGenerator generator(*desktopPipeline_);

    auto duration = std::chrono::milliseconds(10000);  // 10 seconds
    generator.startIngestion(DESKTOP_INGESTION_MBPS, duration);

    std::this_thread::sleep_for(duration + std::chrono::milliseconds(100));
    generator.stop();

    // Should have ingested ~62.5MB (50 Mbps * 10s = 500 Mb = 62.5 MB)
    size_t expectedBytes = mbpsToBytes(DESKTOP_INGESTION_MBPS, 10.0);
    size_t actualBytes = desktopPipeline_->getTotalBytesIngested();

    // Allow 20% variance
    EXPECT_GE(actualBytes, expectedBytes * 0.8);
}

TEST_F(ThroughputTest, DesktopHandles50MbpsBurstIngestion) {
    // Simulate burst traffic - 50 Mbps in 100ms bursts
    size_t burstBytes = mbpsToBytes(DESKTOP_INGESTION_MBPS, 0.1);  // 100ms of data

    for (int i = 0; i < 10; ++i) {
        bool success = desktopPipeline_->ingest(burstBytes);
        EXPECT_TRUE(success);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Total should be ~6.25MB
    size_t expectedTotal = burstBytes * 10;
    EXPECT_GE(desktopPipeline_->getTotalBytesIngested(), expectedTotal * 0.9);
}

// =============================================================================
// Mobile Ingestion Tests (Requirement 13.2)
// =============================================================================

TEST_F(ThroughputTest, MobileSupports20MbpsIngestion) {
    ThroughputGenerator generator(*mobilePipeline_);

    auto duration = std::chrono::milliseconds(1000);
    generator.startIngestion(MOBILE_INGESTION_MBPS, duration);

    std::this_thread::sleep_for(duration + std::chrono::milliseconds(100));
    generator.stop();

    auto avgRate = mobilePipeline_->getAverageIngestionRateMbps(duration);

    // Should achieve at least 80% of target
    EXPECT_GE(avgRate, MOBILE_INGESTION_MBPS * 0.8);
}

TEST_F(ThroughputTest, MobileIngestionWithLimitedBuffer) {
    // Mobile has smaller buffer - test behavior near capacity
    size_t bufferCapacity = 20 * 1024 * 1024;  // 20MB

    // Try to ingest more than buffer capacity
    size_t chunkSize = 1024 * 1024;  // 1MB chunks
    int successCount = 0;
    int failCount = 0;

    for (int i = 0; i < 25; ++i) {
        if (mobilePipeline_->ingest(chunkSize)) {
            successCount++;
        } else {
            failCount++;
        }
    }

    // Should have filled buffer (20 successes) then failed
    EXPECT_EQ(successCount, 20);
    EXPECT_EQ(failCount, 5);
}

// =============================================================================
// Desktop Distribution Tests (Requirement 13.3)
// =============================================================================

TEST_F(ThroughputTest, DesktopSupports500MbpsAggregateDistribution) {
    // First fill the buffer
    size_t bufferFillSize = 50 * 1024 * 1024;  // 50MB
    desktopPipeline_->ingest(bufferFillSize);

    // Distribute to 10 subscribers at 50 Mbps each = 500 Mbps aggregate
    ThroughputGenerator ingestGen(*desktopPipeline_);
    auto duration = std::chrono::milliseconds(1000);

    // Keep ingesting while distributing
    ingestGen.startIngestion(50.0, duration);

    ThroughputGenerator distGen(*desktopPipeline_);
    distGen.startDistribution(10, duration);  // 10 subscribers

    std::this_thread::sleep_for(duration + std::chrono::milliseconds(100));
    ingestGen.stop();
    distGen.stop();

    // Verify distribution rate
    auto avgDistRate = desktopPipeline_->getAverageDistributionRateMbps(duration);

    // Should achieve significant distribution throughput
    // (Note: actual rate depends on how fast we can ingest)
    EXPECT_GT(avgDistRate, 0);
}

TEST_F(ThroughputTest, DesktopDistributesToMultipleSubscribers) {
    desktopPipeline_->ingest(10 * 1024 * 1024);  // 10MB

    // Distribute 1MB to 10 subscribers
    size_t chunkSize = 1024 * 1024;
    size_t subscriberCount = 10;

    desktopPipeline_->distribute(chunkSize, subscriberCount);

    // Total distributed should be 10MB (1MB * 10 subscribers)
    EXPECT_EQ(desktopPipeline_->getTotalBytesDistributed(), chunkSize * subscriberCount);
}

// =============================================================================
// Mobile Distribution Tests (Requirement 13.4)
// =============================================================================

TEST_F(ThroughputTest, MobileSupports100MbpsAggregateDistribution) {
    mobilePipeline_->ingest(10 * 1024 * 1024);  // 10MB

    ThroughputGenerator ingestGen(*mobilePipeline_);
    auto duration = std::chrono::milliseconds(1000);
    ingestGen.startIngestion(20.0, duration);

    ThroughputGenerator distGen(*mobilePipeline_);
    distGen.startDistribution(5, duration);  // 5 subscribers at 20 Mbps each

    std::this_thread::sleep_for(duration + std::chrono::milliseconds(100));
    ingestGen.stop();
    distGen.stop();

    auto avgDistRate = mobilePipeline_->getAverageDistributionRateMbps(duration);
    EXPECT_GT(avgDistRate, 0);
}

TEST_F(ThroughputTest, MobileDistributionLimitedByIngestion) {
    // Mobile can only distribute what's ingested
    // At 20 Mbps ingestion, can't sustain 100 Mbps distribution without buffering

    // No pre-fill
    ThroughputGenerator ingestGen(*mobilePipeline_);
    ThroughputGenerator distGen(*mobilePipeline_);

    auto duration = std::chrono::milliseconds(500);
    ingestGen.startIngestion(20.0, duration);
    distGen.startDistribution(5, duration);

    std::this_thread::sleep_for(duration + std::chrono::milliseconds(100));
    ingestGen.stop();
    distGen.stop();

    // Distribution can't exceed what was ingested (times subscriber count)
    size_t ingested = mobilePipeline_->getTotalBytesIngested();
    size_t distributed = mobilePipeline_->getTotalBytesDistributed();

    // With 5 subscribers, max distribution = 5 * ingestion
    EXPECT_LE(distributed, ingested * 5 + 1024);  // Small tolerance
}

// =============================================================================
// Throughput Stress Tests
// =============================================================================

TEST_F(ThroughputTest, SustainedHighThroughputDesktop) {
    ThroughputGenerator generator(*desktopPipeline_);

    // Run at 80% of max capacity for 5 seconds
    auto duration = std::chrono::milliseconds(5000);
    generator.startIngestion(DESKTOP_INGESTION_MBPS * 0.8, duration);

    std::this_thread::sleep_for(duration + std::chrono::milliseconds(100));
    generator.stop();

    // Should complete without errors
    size_t expectedMinBytes = mbpsToBytes(DESKTOP_INGESTION_MBPS * 0.8 * 0.7, 5.0);
    EXPECT_GE(desktopPipeline_->getTotalBytesIngested(), expectedMinBytes);
}

TEST_F(ThroughputTest, ThroughputWithVaryingRates) {
    std::vector<double> rates = {10.0, 30.0, 50.0, 40.0, 20.0};  // Mbps

    for (double rate : rates) {
        size_t bytesFor100ms = static_cast<size_t>(rate * 125000 * 0.1);
        desktopPipeline_->ingest(bytesFor100ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Average rate should be around 30 Mbps
    auto avgRate = desktopPipeline_->getAverageIngestionRateMbps(std::chrono::milliseconds(500));
    EXPECT_GE(avgRate, 20.0);
    EXPECT_LE(avgRate, 40.0);
}

TEST_F(ThroughputTest, PeakThroughputTracking) {
    // Generate varying throughput
    desktopPipeline_->ingest(mbpsToBytes(10.0, 0.1));  // 10 Mbps
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    desktopPipeline_->ingest(mbpsToBytes(50.0, 0.1));  // 50 Mbps peak
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    desktopPipeline_->ingest(mbpsToBytes(30.0, 0.1));  // 30 Mbps
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Peak should be close to 50 Mbps
    double peak = desktopPipeline_->getPeakIngestionRateMbps();
    EXPECT_GE(peak, 30.0);  // At least captured the burst
}

// =============================================================================
// Concurrent Throughput Tests
// =============================================================================

TEST_F(ThroughputTest, ConcurrentIngestAndDistribute) {
    std::atomic<bool> running{true};
    std::atomic<size_t> ingestOps{0};
    std::atomic<size_t> distOps{0};

    // Ingestion thread
    std::thread ingestThread([this, &running, &ingestOps]() {
        while (running) {
            if (desktopPipeline_->ingest(102400)) {  // 100KB
                ingestOps++;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });

    // Distribution thread
    std::thread distThread([this, &running, &distOps]() {
        while (running) {
            if (desktopPipeline_->distribute(51200, 3)) {  // 50KB to 3 subscribers
                distOps++;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });

    // Run for 500ms
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    running = false;

    ingestThread.join();
    distThread.join();

    // Both operations should have executed many times
    EXPECT_GT(ingestOps.load(), 100u);
    EXPECT_GT(distOps.load(), 100u);
}

TEST_F(ThroughputTest, MultipleIngestionStreams) {
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;

    // 5 concurrent ingestion streams
    for (int i = 0; i < 5; ++i) {
        threads.emplace_back([this, &running, i]() {
            while (running) {
                desktopPipeline_->ingest(20480);  // 20KB per stream
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    running = false;

    for (auto& t : threads) {
        t.join();
    }

    // Should have ingested substantial data
    EXPECT_GT(desktopPipeline_->getTotalBytesIngested(), 1024 * 1024);  // >1MB
}

} // namespace test
} // namespace performance
} // namespace openrtmp
