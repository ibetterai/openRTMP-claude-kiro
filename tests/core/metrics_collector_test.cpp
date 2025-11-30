// OpenRTMP - Cross-platform RTMP Server
// Tests for Metrics Collection Component
//
// Requirements Covered:
// - 12.6: Expose latency metrics through API including ingestion and distribution delays
// - 13.5: Provide throughput metrics through API including bitrate per stream
// - 18.3: Expose metrics through programmatic API (connections, streams, bandwidth, errors)

#include <gtest/gtest.h>
#include "openrtmp/core/metrics_collector.hpp"

#include <chrono>
#include <thread>
#include <vector>

namespace openrtmp {
namespace core {
namespace test {

// =============================================================================
// MetricsCollector Basic Tests
// =============================================================================

class MetricsCollectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        collector_ = std::make_unique<MetricsCollector>();
    }

    void TearDown() override {
        collector_.reset();
    }

    std::unique_ptr<MetricsCollector> collector_;
};

// =============================================================================
// Counter Metrics Tests (Requirement 18.3)
// =============================================================================

TEST_F(MetricsCollectorTest, TracksActiveConnections) {
    // Initially no connections
    EXPECT_EQ(collector_->getActiveConnections(), 0u);

    // Add connections
    collector_->incrementConnections();
    EXPECT_EQ(collector_->getActiveConnections(), 1u);

    collector_->incrementConnections();
    EXPECT_EQ(collector_->getActiveConnections(), 2u);

    // Remove connections
    collector_->decrementConnections();
    EXPECT_EQ(collector_->getActiveConnections(), 1u);
}

TEST_F(MetricsCollectorTest, TracksActiveStreams) {
    // Initially no streams
    EXPECT_EQ(collector_->getActiveStreams(), 0u);

    // Add streams
    collector_->incrementStreams();
    EXPECT_EQ(collector_->getActiveStreams(), 1u);

    collector_->incrementStreams();
    EXPECT_EQ(collector_->getActiveStreams(), 2u);

    // Remove stream
    collector_->decrementStreams();
    EXPECT_EQ(collector_->getActiveStreams(), 1u);
}

TEST_F(MetricsCollectorTest, TracksBytesInAndOut) {
    // Initially zero
    EXPECT_EQ(collector_->getTotalBytesIn(), 0u);
    EXPECT_EQ(collector_->getTotalBytesOut(), 0u);

    // Add bytes
    collector_->addBytesIn(1000);
    collector_->addBytesIn(500);
    EXPECT_EQ(collector_->getTotalBytesIn(), 1500u);

    collector_->addBytesOut(2000);
    collector_->addBytesOut(300);
    EXPECT_EQ(collector_->getTotalBytesOut(), 2300u);
}

TEST_F(MetricsCollectorTest, TracksErrorsByCategory) {
    // Initially no errors
    EXPECT_EQ(collector_->getErrorCount(ErrorCategory::Protocol), 0u);
    EXPECT_EQ(collector_->getErrorCount(ErrorCategory::Network), 0u);
    EXPECT_EQ(collector_->getErrorCount(ErrorCategory::Authentication), 0u);
    EXPECT_EQ(collector_->getErrorCount(ErrorCategory::Resource), 0u);
    EXPECT_EQ(collector_->getErrorCount(ErrorCategory::Internal), 0u);

    // Add errors
    collector_->recordError(ErrorCategory::Protocol);
    collector_->recordError(ErrorCategory::Protocol);
    collector_->recordError(ErrorCategory::Network);
    collector_->recordError(ErrorCategory::Authentication);

    EXPECT_EQ(collector_->getErrorCount(ErrorCategory::Protocol), 2u);
    EXPECT_EQ(collector_->getErrorCount(ErrorCategory::Network), 1u);
    EXPECT_EQ(collector_->getErrorCount(ErrorCategory::Authentication), 1u);
    EXPECT_EQ(collector_->getErrorCount(ErrorCategory::Resource), 0u);
}

TEST_F(MetricsCollectorTest, ReportsTotalErrorCount) {
    collector_->recordError(ErrorCategory::Protocol);
    collector_->recordError(ErrorCategory::Network);
    collector_->recordError(ErrorCategory::Authentication);
    collector_->recordError(ErrorCategory::Resource);
    collector_->recordError(ErrorCategory::Internal);

    EXPECT_EQ(collector_->getTotalErrorCount(), 5u);
}

// =============================================================================
// Gauge Metrics Tests (Requirement 18.3)
// =============================================================================

TEST_F(MetricsCollectorTest, TracksCurrentBandwidthUsage) {
    // Set bandwidth usage
    collector_->setCurrentBandwidthIn(5000000);  // 5 Mbps
    collector_->setCurrentBandwidthOut(10000000);  // 10 Mbps

    EXPECT_EQ(collector_->getCurrentBandwidthIn(), 5000000u);
    EXPECT_EQ(collector_->getCurrentBandwidthOut(), 10000000u);
}

TEST_F(MetricsCollectorTest, ConnectionCountDoesNotGoNegative) {
    EXPECT_EQ(collector_->getActiveConnections(), 0u);

    // Try to decrement below zero
    collector_->decrementConnections();

    // Should still be 0
    EXPECT_EQ(collector_->getActiveConnections(), 0u);
}

TEST_F(MetricsCollectorTest, StreamCountDoesNotGoNegative) {
    EXPECT_EQ(collector_->getActiveStreams(), 0u);

    // Try to decrement below zero
    collector_->decrementStreams();

    // Should still be 0
    EXPECT_EQ(collector_->getActiveStreams(), 0u);
}

// =============================================================================
// Per-Stream Metrics Tests (Requirements 12.6, 13.5)
// =============================================================================

TEST_F(MetricsCollectorTest, TracksPerStreamMetrics) {
    StreamKey key1{"live", "stream1"};
    StreamKey key2{"live", "stream2"};

    // Create stream metrics
    collector_->createStreamMetrics(key1);
    collector_->createStreamMetrics(key2);

    // Should have two streams tracked
    auto allMetrics = collector_->getAllStreamMetrics();
    EXPECT_EQ(allMetrics.size(), 2u);
}

TEST_F(MetricsCollectorTest, TracksStreamIngestionDelay) {
    StreamKey key{"live", "teststream"};
    collector_->createStreamMetrics(key);

    // Record ingestion delay
    collector_->recordIngestionDelay(key, std::chrono::milliseconds(50));
    collector_->recordIngestionDelay(key, std::chrono::milliseconds(60));
    collector_->recordIngestionDelay(key, std::chrono::milliseconds(40));

    auto metrics = collector_->getStreamMetrics(key);
    ASSERT_TRUE(metrics.has_value());

    // Average should be 50ms
    EXPECT_EQ(metrics->averageIngestionDelay.count(), 50);
}

TEST_F(MetricsCollectorTest, TracksStreamDistributionDelay) {
    StreamKey key{"live", "teststream"};
    collector_->createStreamMetrics(key);

    // Record distribution delay
    collector_->recordDistributionDelay(key, std::chrono::milliseconds(30));
    collector_->recordDistributionDelay(key, std::chrono::milliseconds(40));
    collector_->recordDistributionDelay(key, std::chrono::milliseconds(50));

    auto metrics = collector_->getStreamMetrics(key);
    ASSERT_TRUE(metrics.has_value());

    // Average should be 40ms
    EXPECT_EQ(metrics->averageDistributionDelay.count(), 40);
}

TEST_F(MetricsCollectorTest, TracksStreamBitrate) {
    StreamKey key{"live", "teststream"};
    collector_->createStreamMetrics(key);

    // Update bitrate
    collector_->updateStreamBitrate(key, 5000000);  // 5 Mbps

    auto metrics = collector_->getStreamMetrics(key);
    ASSERT_TRUE(metrics.has_value());
    EXPECT_EQ(metrics->currentBitrate, 5000000u);
}

TEST_F(MetricsCollectorTest, TracksStreamBytesInOut) {
    StreamKey key{"live", "teststream"};
    collector_->createStreamMetrics(key);

    // Add bytes
    collector_->addStreamBytesIn(key, 1000);
    collector_->addStreamBytesIn(key, 2000);
    collector_->addStreamBytesOut(key, 5000);

    auto metrics = collector_->getStreamMetrics(key);
    ASSERT_TRUE(metrics.has_value());
    EXPECT_EQ(metrics->bytesIn, 3000u);
    EXPECT_EQ(metrics->bytesOut, 5000u);
}

TEST_F(MetricsCollectorTest, RemovesStreamMetrics) {
    StreamKey key{"live", "teststream"};
    collector_->createStreamMetrics(key);

    // Verify stream exists
    EXPECT_TRUE(collector_->getStreamMetrics(key).has_value());

    // Remove stream
    collector_->removeStreamMetrics(key);

    // Verify stream no longer exists
    EXPECT_FALSE(collector_->getStreamMetrics(key).has_value());
}

TEST_F(MetricsCollectorTest, ReturnsEmptyOptionalForUnknownStream) {
    StreamKey unknownKey{"live", "unknown"};

    auto metrics = collector_->getStreamMetrics(unknownKey);
    EXPECT_FALSE(metrics.has_value());
}

// =============================================================================
// Latency Histogram Tests (Requirement 12.6)
// =============================================================================

TEST_F(MetricsCollectorTest, TracksLatencyDistribution) {
    // Record various latencies
    collector_->recordLatency(std::chrono::milliseconds(10));
    collector_->recordLatency(std::chrono::milliseconds(25));
    collector_->recordLatency(std::chrono::milliseconds(50));
    collector_->recordLatency(std::chrono::milliseconds(100));
    collector_->recordLatency(std::chrono::milliseconds(200));

    auto histogram = collector_->getLatencyHistogram();

    // Check that we have histogram buckets
    EXPECT_FALSE(histogram.buckets.empty());

    // Total count should match
    EXPECT_EQ(histogram.totalCount, 5u);
}

TEST_F(MetricsCollectorTest, CalculatesLatencyPercentiles) {
    // Record multiple latencies
    for (int i = 1; i <= 100; ++i) {
        collector_->recordLatency(std::chrono::milliseconds(i));
    }

    auto histogram = collector_->getLatencyHistogram();

    // P50 should be around 50ms
    EXPECT_GE(histogram.p50.count(), 45);
    EXPECT_LE(histogram.p50.count(), 55);

    // P95 should be around 95ms
    EXPECT_GE(histogram.p95.count(), 90);
    EXPECT_LE(histogram.p95.count(), 100);

    // P99 should be around 99ms
    EXPECT_GE(histogram.p99.count(), 95);
    EXPECT_LE(histogram.p99.count(), 100);
}

// =============================================================================
// Bitrate Histogram Tests (Requirement 13.5)
// =============================================================================

TEST_F(MetricsCollectorTest, TracksBitrateDistribution) {
    // Record various bitrates
    collector_->recordBitrate(1000000);   // 1 Mbps
    collector_->recordBitrate(2000000);   // 2 Mbps
    collector_->recordBitrate(5000000);   // 5 Mbps
    collector_->recordBitrate(10000000);  // 10 Mbps

    auto histogram = collector_->getBitrateHistogram();

    EXPECT_EQ(histogram.totalCount, 4u);
    EXPECT_FALSE(histogram.buckets.empty());
}

// =============================================================================
// Snapshot and Reset Tests
// =============================================================================

TEST_F(MetricsCollectorTest, TakesMetricsSnapshot) {
    // Populate some metrics
    collector_->incrementConnections();
    collector_->incrementConnections();
    collector_->incrementStreams();
    collector_->addBytesIn(1000);
    collector_->addBytesOut(2000);
    collector_->recordError(ErrorCategory::Protocol);

    auto snapshot = collector_->takeSnapshot();

    EXPECT_EQ(snapshot.activeConnections, 2u);
    EXPECT_EQ(snapshot.activeStreams, 1u);
    EXPECT_EQ(snapshot.totalBytesIn, 1000u);
    EXPECT_EQ(snapshot.totalBytesOut, 2000u);
    EXPECT_EQ(snapshot.totalErrors, 1u);
}

TEST_F(MetricsCollectorTest, ResetsCounters) {
    // Populate some metrics
    collector_->incrementConnections();
    collector_->addBytesIn(1000);
    collector_->recordError(ErrorCategory::Protocol);

    // Reset
    collector_->reset();

    // All counters should be zero
    EXPECT_EQ(collector_->getActiveConnections(), 0u);
    EXPECT_EQ(collector_->getTotalBytesIn(), 0u);
    EXPECT_EQ(collector_->getTotalErrorCount(), 0u);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST_F(MetricsCollectorTest, ThreadSafeCounterUpdates) {
    const int numThreads = 10;
    const int incrementsPerThread = 1000;

    std::vector<std::thread> threads;
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this]() {
            for (int j = 0; j < 1000; ++j) {
                collector_->incrementConnections();
                collector_->addBytesIn(1);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(collector_->getActiveConnections(),
              static_cast<uint64_t>(numThreads * incrementsPerThread));
    EXPECT_EQ(collector_->getTotalBytesIn(),
              static_cast<uint64_t>(numThreads * incrementsPerThread));
}

TEST_F(MetricsCollectorTest, ThreadSafeStreamMetrics) {
    StreamKey key{"live", "concurrent"};
    collector_->createStreamMetrics(key);

    const int numThreads = 4;

    std::vector<std::thread> threads;
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, &key]() {
            for (int j = 0; j < 100; ++j) {
                collector_->addStreamBytesIn(key, 10);
                collector_->recordIngestionDelay(key, std::chrono::milliseconds(50));
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto metrics = collector_->getStreamMetrics(key);
    ASSERT_TRUE(metrics.has_value());
    EXPECT_EQ(metrics->bytesIn, static_cast<uint64_t>(numThreads * 100 * 10));
}

// =============================================================================
// Export Format Tests (Optional: Prometheus, JSON)
// =============================================================================

TEST_F(MetricsCollectorTest, ExportsToPrometheusFormat) {
    collector_->incrementConnections();
    collector_->incrementConnections();
    collector_->incrementStreams();
    collector_->addBytesIn(1000);
    collector_->addBytesOut(2000);

    std::string prometheus = collector_->exportPrometheus();

    // Should contain metric names and values
    EXPECT_NE(prometheus.find("openrtmp_connections_active"), std::string::npos);
    EXPECT_NE(prometheus.find("openrtmp_streams_active"), std::string::npos);
    EXPECT_NE(prometheus.find("openrtmp_bytes_in_total"), std::string::npos);
    EXPECT_NE(prometheus.find("openrtmp_bytes_out_total"), std::string::npos);
}

TEST_F(MetricsCollectorTest, ExportsToJsonFormat) {
    collector_->incrementConnections();
    collector_->incrementStreams();

    std::string json = collector_->exportJson();

    // Should be valid JSON-like format
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
    EXPECT_NE(json.find("\"activeConnections\""), std::string::npos);
    EXPECT_NE(json.find("\"activeStreams\""), std::string::npos);
}

// =============================================================================
// Uptime Tests
// =============================================================================

TEST_F(MetricsCollectorTest, TracksUptime) {
    // Wait a small amount
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto uptime = collector_->getUptime();
    EXPECT_GE(uptime.count(), 10);
}

// =============================================================================
// Error Category String Conversion Tests
// =============================================================================

TEST_F(MetricsCollectorTest, ErrorCategoryToString) {
    EXPECT_EQ(errorCategoryToString(ErrorCategory::Protocol), "protocol");
    EXPECT_EQ(errorCategoryToString(ErrorCategory::Network), "network");
    EXPECT_EQ(errorCategoryToString(ErrorCategory::Authentication), "authentication");
    EXPECT_EQ(errorCategoryToString(ErrorCategory::Resource), "resource");
    EXPECT_EQ(errorCategoryToString(ErrorCategory::Internal), "internal");
}

// =============================================================================
// Stream Subscriber Metrics Tests
// =============================================================================

TEST_F(MetricsCollectorTest, TracksStreamSubscriberCount) {
    StreamKey key{"live", "teststream"};
    collector_->createStreamMetrics(key);

    collector_->incrementStreamSubscribers(key);
    collector_->incrementStreamSubscribers(key);

    auto metrics = collector_->getStreamMetrics(key);
    ASSERT_TRUE(metrics.has_value());
    EXPECT_EQ(metrics->subscriberCount, 2u);

    collector_->decrementStreamSubscribers(key);
    metrics = collector_->getStreamMetrics(key);
    EXPECT_EQ(metrics->subscriberCount, 1u);
}

// =============================================================================
// Peak Value Tests
// =============================================================================

TEST_F(MetricsCollectorTest, TracksPeakConnections) {
    collector_->incrementConnections();
    collector_->incrementConnections();
    collector_->incrementConnections();  // Peak = 3

    collector_->decrementConnections();  // Current = 2

    auto snapshot = collector_->takeSnapshot();
    EXPECT_EQ(snapshot.activeConnections, 2u);
    EXPECT_EQ(snapshot.peakConnections, 3u);
}

TEST_F(MetricsCollectorTest, TracksPeakStreams) {
    collector_->incrementStreams();
    collector_->incrementStreams();  // Peak = 2

    collector_->decrementStreams();  // Current = 1

    auto snapshot = collector_->takeSnapshot();
    EXPECT_EQ(snapshot.activeStreams, 1u);
    EXPECT_EQ(snapshot.peakStreams, 2u);
}

} // namespace test
} // namespace core
} // namespace openrtmp
