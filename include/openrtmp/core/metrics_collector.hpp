// OpenRTMP - Cross-platform RTMP Server
// Metrics Collection Component
//
// Provides metrics collection and exposure through programmatic API.
// Supports counter, gauge, and histogram metrics for monitoring.
//
// Requirements Covered:
// - 12.6: Expose latency metrics through API (ingestion/distribution delays)
// - 13.5: Provide throughput metrics through API (bitrate per stream)
// - 18.3: Expose metrics through programmatic API (connections, streams, bandwidth, errors)

#ifndef OPENRTMP_CORE_METRICS_COLLECTOR_HPP
#define OPENRTMP_CORE_METRICS_COLLECTOR_HPP

#include "openrtmp/core/types.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace openrtmp {
namespace core {

/**
 * @brief Error categories for tracking errors by type per requirement 18.3.
 */
enum class ErrorCategory {
    Protocol,       ///< RTMP protocol errors (malformed packets, invalid state)
    Network,        ///< Network-related errors (connection failures, timeouts)
    Authentication, ///< Authentication failures
    Resource,       ///< Resource exhaustion (memory, connections)
    Internal        ///< Internal server errors
};

/**
 * @brief Convert error category to string representation.
 */
std::string errorCategoryToString(ErrorCategory category);

/**
 * @brief Per-stream metrics structure per requirements 12.6, 13.5.
 *
 * Contains metrics for individual streams including:
 * - Ingestion and distribution delays
 * - Bitrate information
 * - Bytes transferred
 * - Subscriber count
 */
struct StreamMetricsData {
    StreamKey key;                                ///< Stream identifier

    // Latency metrics (Requirement 12.6)
    std::chrono::milliseconds averageIngestionDelay{0};   ///< Average time from receipt to buffer
    std::chrono::milliseconds averageDistributionDelay{0}; ///< Average time from buffer to subscribers

    // Throughput metrics (Requirement 13.5)
    uint64_t currentBitrate = 0;    ///< Current bitrate in bits per second
    uint64_t bytesIn = 0;           ///< Total bytes received for this stream
    uint64_t bytesOut = 0;          ///< Total bytes distributed from this stream

    // Subscriber metrics
    uint32_t subscriberCount = 0;   ///< Current number of subscribers

    // Timing
    std::chrono::steady_clock::time_point startTime;  ///< When stream started
};

/**
 * @brief Latency histogram structure for percentile calculations.
 */
struct LatencyHistogram {
    std::vector<std::pair<std::chrono::milliseconds, uint64_t>> buckets; ///< Bucket boundaries and counts
    uint64_t totalCount = 0;         ///< Total number of samples
    std::chrono::milliseconds p50{0}; ///< 50th percentile
    std::chrono::milliseconds p95{0}; ///< 95th percentile
    std::chrono::milliseconds p99{0}; ///< 99th percentile
    std::chrono::milliseconds min{0}; ///< Minimum latency
    std::chrono::milliseconds max{0}; ///< Maximum latency
};

/**
 * @brief Bitrate histogram structure for throughput analysis.
 */
struct BitrateHistogram {
    std::vector<std::pair<uint64_t, uint64_t>> buckets; ///< Bucket boundaries (bps) and counts
    uint64_t totalCount = 0;         ///< Total number of samples
    uint64_t average = 0;            ///< Average bitrate
    uint64_t min = 0;                ///< Minimum bitrate
    uint64_t max = 0;                ///< Maximum bitrate
};

/**
 * @brief Metrics snapshot for atomic retrieval of all metrics.
 */
struct MetricsSnapshot {
    // Connection metrics (Requirement 18.3)
    uint64_t activeConnections = 0;
    uint64_t peakConnections = 0;
    uint64_t totalConnections = 0;  ///< All-time connections

    // Stream metrics
    uint64_t activeStreams = 0;
    uint64_t peakStreams = 0;
    uint64_t totalStreams = 0;      ///< All-time streams

    // Bandwidth metrics (Requirement 18.3)
    uint64_t totalBytesIn = 0;
    uint64_t totalBytesOut = 0;
    uint64_t currentBandwidthIn = 0;   ///< Current ingestion bandwidth (bps)
    uint64_t currentBandwidthOut = 0;  ///< Current distribution bandwidth (bps)

    // Error metrics (Requirement 18.3)
    uint64_t totalErrors = 0;
    std::map<ErrorCategory, uint64_t> errorsByCategory;

    // Timing
    std::chrono::milliseconds uptime{0};
    std::chrono::system_clock::time_point snapshotTime;
};

/**
 * @brief Metrics collector for server-wide and per-stream metrics.
 *
 * This component provides comprehensive metrics collection including:
 * - Counter metrics: connections, bytes, errors
 * - Gauge metrics: active connections, streams, bandwidth
 * - Histogram metrics: latency distributions, bitrate distributions
 * - Per-stream metrics: ingestion delay, distribution delay, bitrate
 *
 * ## Thread Safety
 * All methods are thread-safe. Metrics can be updated from multiple threads
 * concurrently without data corruption.
 *
 * ## Usage Example
 * @code
 * auto collector = std::make_shared<MetricsCollector>();
 *
 * // Track connections
 * collector->incrementConnections();
 * collector->addBytesIn(1024);
 *
 * // Track stream metrics
 * StreamKey key{"live", "mystream"};
 * collector->createStreamMetrics(key);
 * collector->recordIngestionDelay(key, std::chrono::milliseconds(50));
 * collector->updateStreamBitrate(key, 5000000);
 *
 * // Get snapshot
 * auto snapshot = collector->takeSnapshot();
 *
 * // Export for monitoring
 * std::string prometheus = collector->exportPrometheus();
 * @endcode
 */
class MetricsCollector {
public:
    /**
     * @brief Default constructor.
     *
     * Initializes the metrics collector with zero values.
     */
    MetricsCollector();

    /**
     * @brief Destructor.
     */
    ~MetricsCollector();

    // Non-copyable, non-movable for thread safety
    MetricsCollector(const MetricsCollector&) = delete;
    MetricsCollector& operator=(const MetricsCollector&) = delete;
    MetricsCollector(MetricsCollector&&) = delete;
    MetricsCollector& operator=(MetricsCollector&&) = delete;

    // =========================================================================
    // Connection Metrics (Requirement 18.3)
    // =========================================================================

    /**
     * @brief Increment active connection count.
     *
     * Call when a new connection is accepted.
     */
    void incrementConnections();

    /**
     * @brief Decrement active connection count.
     *
     * Call when a connection is closed.
     */
    void decrementConnections();

    /**
     * @brief Get current active connection count.
     *
     * @return Number of active connections
     */
    uint64_t getActiveConnections() const;

    // =========================================================================
    // Stream Metrics (Requirement 18.3)
    // =========================================================================

    /**
     * @brief Increment active stream count.
     *
     * Call when a new stream starts publishing.
     */
    void incrementStreams();

    /**
     * @brief Decrement active stream count.
     *
     * Call when a stream ends.
     */
    void decrementStreams();

    /**
     * @brief Get current active stream count.
     *
     * @return Number of active streams
     */
    uint64_t getActiveStreams() const;

    // =========================================================================
    // Bandwidth Metrics (Requirement 18.3)
    // =========================================================================

    /**
     * @brief Add bytes received (ingestion).
     *
     * @param bytes Number of bytes received
     */
    void addBytesIn(uint64_t bytes);

    /**
     * @brief Add bytes sent (distribution).
     *
     * @param bytes Number of bytes sent
     */
    void addBytesOut(uint64_t bytes);

    /**
     * @brief Get total bytes received.
     *
     * @return Total bytes received since start
     */
    uint64_t getTotalBytesIn() const;

    /**
     * @brief Get total bytes sent.
     *
     * @return Total bytes sent since start
     */
    uint64_t getTotalBytesOut() const;

    /**
     * @brief Set current bandwidth in (updated periodically).
     *
     * @param bps Current ingestion bandwidth in bits per second
     */
    void setCurrentBandwidthIn(uint64_t bps);

    /**
     * @brief Set current bandwidth out (updated periodically).
     *
     * @param bps Current distribution bandwidth in bits per second
     */
    void setCurrentBandwidthOut(uint64_t bps);

    /**
     * @brief Get current ingestion bandwidth.
     *
     * @return Current bandwidth in bits per second
     */
    uint64_t getCurrentBandwidthIn() const;

    /**
     * @brief Get current distribution bandwidth.
     *
     * @return Current bandwidth in bits per second
     */
    uint64_t getCurrentBandwidthOut() const;

    // =========================================================================
    // Error Metrics (Requirement 18.3)
    // =========================================================================

    /**
     * @brief Record an error by category.
     *
     * @param category Error category
     */
    void recordError(ErrorCategory category);

    /**
     * @brief Get error count for a specific category.
     *
     * @param category Error category
     * @return Number of errors in that category
     */
    uint64_t getErrorCount(ErrorCategory category) const;

    /**
     * @brief Get total error count across all categories.
     *
     * @return Total error count
     */
    uint64_t getTotalErrorCount() const;

    // =========================================================================
    // Per-Stream Metrics (Requirements 12.6, 13.5)
    // =========================================================================

    /**
     * @brief Create metrics tracking for a stream.
     *
     * @param key Stream key
     */
    void createStreamMetrics(const StreamKey& key);

    /**
     * @brief Remove metrics tracking for a stream.
     *
     * @param key Stream key
     */
    void removeStreamMetrics(const StreamKey& key);

    /**
     * @brief Get metrics for a specific stream.
     *
     * @param key Stream key
     * @return Stream metrics if found, empty optional otherwise
     */
    std::optional<StreamMetricsData> getStreamMetrics(const StreamKey& key) const;

    /**
     * @brief Get metrics for all streams.
     *
     * @return Map of stream key to metrics
     */
    std::unordered_map<StreamKey, StreamMetricsData> getAllStreamMetrics() const;

    /**
     * @brief Record ingestion delay for a stream (Requirement 12.6).
     *
     * @param key Stream key
     * @param delay Ingestion delay
     */
    void recordIngestionDelay(const StreamKey& key, std::chrono::milliseconds delay);

    /**
     * @brief Record distribution delay for a stream (Requirement 12.6).
     *
     * @param key Stream key
     * @param delay Distribution delay
     */
    void recordDistributionDelay(const StreamKey& key, std::chrono::milliseconds delay);

    /**
     * @brief Update bitrate for a stream (Requirement 13.5).
     *
     * @param key Stream key
     * @param bitrate Current bitrate in bits per second
     */
    void updateStreamBitrate(const StreamKey& key, uint64_t bitrate);

    /**
     * @brief Add bytes received for a stream.
     *
     * @param key Stream key
     * @param bytes Number of bytes
     */
    void addStreamBytesIn(const StreamKey& key, uint64_t bytes);

    /**
     * @brief Add bytes sent for a stream.
     *
     * @param key Stream key
     * @param bytes Number of bytes
     */
    void addStreamBytesOut(const StreamKey& key, uint64_t bytes);

    /**
     * @brief Increment subscriber count for a stream.
     *
     * @param key Stream key
     */
    void incrementStreamSubscribers(const StreamKey& key);

    /**
     * @brief Decrement subscriber count for a stream.
     *
     * @param key Stream key
     */
    void decrementStreamSubscribers(const StreamKey& key);

    // =========================================================================
    // Histogram Metrics (Requirement 12.6)
    // =========================================================================

    /**
     * @brief Record a latency sample for global histogram.
     *
     * @param latency Latency value
     */
    void recordLatency(std::chrono::milliseconds latency);

    /**
     * @brief Get latency histogram with percentiles.
     *
     * @return Latency histogram data
     */
    LatencyHistogram getLatencyHistogram() const;

    /**
     * @brief Record a bitrate sample for global histogram.
     *
     * @param bitrate Bitrate in bits per second
     */
    void recordBitrate(uint64_t bitrate);

    /**
     * @brief Get bitrate histogram.
     *
     * @return Bitrate histogram data
     */
    BitrateHistogram getBitrateHistogram() const;

    // =========================================================================
    // Snapshot and Export
    // =========================================================================

    /**
     * @brief Take an atomic snapshot of all metrics.
     *
     * @return Complete metrics snapshot
     */
    MetricsSnapshot takeSnapshot() const;

    /**
     * @brief Reset all counters to zero.
     *
     * Note: This does not affect stream metrics.
     */
    void reset();

    /**
     * @brief Get server uptime.
     *
     * @return Duration since collector was created
     */
    std::chrono::milliseconds getUptime() const;

    /**
     * @brief Export metrics in Prometheus text format.
     *
     * @return Prometheus-formatted metrics string
     */
    std::string exportPrometheus() const;

    /**
     * @brief Export metrics in JSON format.
     *
     * @return JSON-formatted metrics string
     */
    std::string exportJson() const;

private:
    // Internal helper methods
    void updatePeakConnections();
    void updatePeakStreams();
    std::chrono::milliseconds calculatePercentile(
        const std::vector<std::chrono::milliseconds>& samples,
        double percentile) const;

    // Start time for uptime calculation
    std::chrono::steady_clock::time_point startTime_;

    // Connection metrics
    std::atomic<uint64_t> activeConnections_{0};
    std::atomic<uint64_t> peakConnections_{0};
    std::atomic<uint64_t> totalConnections_{0};

    // Stream metrics
    std::atomic<uint64_t> activeStreams_{0};
    std::atomic<uint64_t> peakStreams_{0};
    std::atomic<uint64_t> totalStreams_{0};

    // Bandwidth metrics
    std::atomic<uint64_t> totalBytesIn_{0};
    std::atomic<uint64_t> totalBytesOut_{0};
    std::atomic<uint64_t> currentBandwidthIn_{0};
    std::atomic<uint64_t> currentBandwidthOut_{0};

    // Error metrics
    mutable std::mutex errorMutex_;
    std::map<ErrorCategory, std::atomic<uint64_t>> errorCounts_;

    // Per-stream metrics
    mutable std::mutex streamMetricsMutex_;
    std::unordered_map<StreamKey, std::shared_ptr<StreamMetricsData>> streamMetrics_;

    // Latency histogram data
    mutable std::mutex latencyMutex_;
    std::vector<std::chrono::milliseconds> latencySamples_;

    // Bitrate histogram data
    mutable std::mutex bitrateMutex_;
    std::vector<uint64_t> bitrateSamples_;

    // Per-stream delay samples for averaging
    struct StreamDelaySamples {
        std::vector<std::chrono::milliseconds> ingestionDelays;
        std::vector<std::chrono::milliseconds> distributionDelays;
    };
    std::unordered_map<StreamKey, StreamDelaySamples> streamDelaySamples_;
};

} // namespace core
} // namespace openrtmp

#endif // OPENRTMP_CORE_METRICS_COLLECTOR_HPP
