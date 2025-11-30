// OpenRTMP - Cross-platform RTMP Server
// Throughput Optimizer - Bitrate measurement, tracking, and optimization
//
// Responsibilities:
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

#ifndef OPENRTMP_STREAMING_THROUGHPUT_OPTIMIZER_HPP
#define OPENRTMP_STREAMING_THROUGHPUT_OPTIMIZER_HPP

#include <cstdint>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace streaming {

// =============================================================================
// Warning Types
// =============================================================================

/**
 * @brief Types of throughput warnings.
 */
enum class ThroughputWarningType {
    IngestionOverLimit,         // Stream ingestion exceeds limit
    DistributionOverLimit,      // Stream distribution exceeds limit
    AggregateIngestionOverLimit,    // Total ingestion exceeds limit
    AggregateDistributionOverLimit  // Total distribution exceeds limit
};

// =============================================================================
// Configuration Types
// =============================================================================

/**
 * @brief Configuration for throughput optimization behavior.
 */
struct ThroughputConfig {
    /// Maximum ingestion bitrate per stream in Mbps (default 50 Mbps for desktop)
    double maxIngestionBitrateMbps{50.0};

    /// Maximum aggregate distribution bitrate in Mbps (default 500 Mbps for desktop)
    /// This is a server-wide limit for all streams combined
    double maxDistributionBitrateMbps{500.0};

    /// Maximum per-stream distribution bitrate (optional, defaults to aggregate limit)
    /// Set to 0 to disable per-stream distribution limit checking
    double maxPerStreamDistributionBitrateMbps{0.0};

    /// Enable warning callbacks when limits are exceeded
    bool warningEnabled{true};

    /// Time window for bitrate smoothing/averaging (default 1 second)
    std::chrono::milliseconds smoothingWindowMs{1000};

    /// Minimum interval between warning callbacks for same stream
    std::chrono::milliseconds warningCooldownMs{5000};

    ThroughputConfig() = default;

    /**
     * @brief Calculate max ingestion bytes per second.
     * @return Maximum ingestion bytes per second
     */
    [[nodiscard]] size_t maxIngestionBytesPerSecond() const {
        return static_cast<size_t>(maxIngestionBitrateMbps * 1000000.0 / 8.0);
    }

    /**
     * @brief Calculate max distribution bytes per second.
     * @return Maximum distribution bytes per second
     */
    [[nodiscard]] size_t maxDistributionBytesPerSecond() const {
        return static_cast<size_t>(maxDistributionBitrateMbps * 1000000.0 / 8.0);
    }

    /**
     * @brief Create configuration for desktop platforms.
     *
     * Desktop limits per requirements:
     * - 50 Mbps ingestion per stream (Requirement 13.1)
     * - 500 Mbps aggregate distribution (Requirement 13.3)
     *
     * @return Desktop-optimized configuration
     */
    static ThroughputConfig forDesktop() {
        ThroughputConfig config;
        config.maxIngestionBitrateMbps = 50.0;
        config.maxDistributionBitrateMbps = 500.0;
        config.maxPerStreamDistributionBitrateMbps = 0.0;  // No per-stream limit
        return config;
    }

    /**
     * @brief Create configuration for mobile platforms.
     *
     * Mobile limits per requirements:
     * - 20 Mbps ingestion per stream (Requirement 13.2)
     * - 100 Mbps aggregate distribution (Requirement 13.4)
     *
     * @return Mobile-optimized configuration
     */
    static ThroughputConfig forMobile() {
        ThroughputConfig config;
        config.maxIngestionBitrateMbps = 20.0;
        config.maxDistributionBitrateMbps = 100.0;
        config.maxPerStreamDistributionBitrateMbps = 0.0;  // No per-stream limit
        return config;
    }

    /**
     * @brief Create configuration for current platform.
     *
     * Automatically selects desktop or mobile configuration based on
     * the current platform detection.
     *
     * @return Platform-appropriate configuration
     */
    static ThroughputConfig forCurrentPlatform() {
        if (core::isDesktopPlatform()) {
            return forDesktop();
        } else if (core::isMobilePlatform()) {
            return forMobile();
        }
        // Default to desktop if unknown
        return forDesktop();
    }
};

// =============================================================================
// Statistics Types
// =============================================================================

/**
 * @brief Statistics for a single stream's throughput.
 */
struct StreamThroughputStatistics {
    /// Stream key
    StreamKey streamKey;

    /// Total bytes ingested
    uint64_t totalIngestionBytes{0};

    /// Total bytes distributed
    uint64_t totalDistributionBytes{0};

    /// Current ingestion bitrate (smoothed)
    double currentIngestionBitrateMbps{0.0};

    /// Current distribution bitrate (smoothed)
    double currentDistributionBitrateMbps{0.0};

    /// Average ingestion bitrate over tracking period
    double averageIngestionBitrateMbps{0.0};

    /// Average distribution bitrate over tracking period
    double averageDistributionBitrateMbps{0.0};

    /// Peak ingestion bitrate observed
    double peakIngestionBitrateMbps{0.0};

    /// Peak distribution bitrate observed
    double peakDistributionBitrateMbps{0.0};

    /// Time when tracking started
    std::chrono::steady_clock::time_point trackingStarted;

    /// Duration of tracking
    std::chrono::milliseconds trackingDurationMs{0};

    /// Whether ingestion is currently over limit
    bool ingestionOverLimit{false};

    /// Whether distribution is currently over limit
    bool distributionOverLimit{false};
};

/**
 * @brief Aggregate throughput statistics across all streams.
 */
struct AggregateThroughputStatistics {
    /// Total bytes ingested across all streams
    uint64_t totalIngestionBytes{0};

    /// Total bytes distributed across all streams
    uint64_t totalDistributionBytes{0};

    /// Current aggregate ingestion bitrate (smoothed)
    double currentIngestionBitrateMbps{0.0};

    /// Current aggregate distribution bitrate (smoothed)
    double currentDistributionBitrateMbps{0.0};

    /// Number of active streams
    size_t activeStreamCount{0};

    /// Whether aggregate ingestion is over limit
    bool ingestionOverLimit{false};

    /// Whether aggregate distribution is over limit
    bool distributionOverLimit{false};
};

// =============================================================================
// Callback Types
// =============================================================================

/**
 * @brief Callback invoked when throughput exceeds configured limits.
 *
 * @param key Stream key (may be empty for aggregate warnings)
 * @param bitrateMbps The recorded bitrate that triggered the warning
 * @param type Type of warning
 */
using OnThroughputWarningCallback = std::function<void(
    const StreamKey& key,
    double bitrateMbps,
    ThroughputWarningType type
)>;

// =============================================================================
// Throughput Optimizer Interface
// =============================================================================

/**
 * @brief Interface for throughput optimization operations.
 */
class IThroughputOptimizer {
public:
    virtual ~IThroughputOptimizer() = default;

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    /**
     * @brief Set throughput configuration.
     *
     * @param config New configuration
     */
    virtual void setConfig(const ThroughputConfig& config) = 0;

    /**
     * @brief Get current configuration.
     *
     * @return Current configuration
     */
    virtual ThroughputConfig getConfig() const = 0;

    // -------------------------------------------------------------------------
    // Stream Tracking
    // -------------------------------------------------------------------------

    /**
     * @brief Start tracking throughput for a stream.
     *
     * @param key Stream key
     */
    virtual void startStreamTracking(const StreamKey& key) = 0;

    /**
     * @brief Stop tracking throughput for a stream.
     *
     * @param key Stream key
     */
    virtual void stopStreamTracking(const StreamKey& key) = 0;

    /**
     * @brief Check if a stream is being tracked.
     *
     * @param key Stream key
     * @return true if stream is being tracked
     */
    virtual bool isTrackingStream(const StreamKey& key) const = 0;

    /**
     * @brief Get all tracked streams.
     *
     * @return Vector of tracked stream keys
     */
    virtual std::vector<StreamKey> getTrackedStreams() const = 0;

    // -------------------------------------------------------------------------
    // Data Recording
    // -------------------------------------------------------------------------

    /**
     * @brief Record data received during ingestion.
     *
     * @param key Stream key
     * @param bytes Number of bytes received
     */
    virtual void recordIngestionData(const StreamKey& key, size_t bytes) = 0;

    /**
     * @brief Record data sent during distribution.
     *
     * @param key Stream key
     * @param bytes Number of bytes sent
     */
    virtual void recordDistributionData(const StreamKey& key, size_t bytes) = 0;

    // -------------------------------------------------------------------------
    // Statistics
    // -------------------------------------------------------------------------

    /**
     * @brief Get throughput statistics for a stream.
     *
     * @param key Stream key
     * @return Statistics or empty if not tracked
     */
    virtual std::optional<StreamThroughputStatistics>
    getStreamStatistics(const StreamKey& key) const = 0;

    /**
     * @brief Get aggregate throughput statistics.
     *
     * @return Aggregate statistics across all streams
     */
    virtual AggregateThroughputStatistics getAggregateThroughput() const = 0;

    // -------------------------------------------------------------------------
    // Limit Checks
    // -------------------------------------------------------------------------

    /**
     * @brief Check if stream ingestion is over limit.
     *
     * @param key Stream key
     * @return true if over limit
     */
    virtual bool isIngestionOverLimit(const StreamKey& key) const = 0;

    /**
     * @brief Check if stream distribution is over limit.
     *
     * @param key Stream key
     * @return true if over limit
     */
    virtual bool isDistributionOverLimit(const StreamKey& key) const = 0;

    /**
     * @brief Check if aggregate distribution is over limit.
     *
     * @return true if over limit
     */
    virtual bool isAggregateDistributionOverLimit() const = 0;

    // -------------------------------------------------------------------------
    // Callbacks
    // -------------------------------------------------------------------------

    /**
     * @brief Set callback for throughput warning events.
     *
     * @param callback Callback function
     */
    virtual void setOnThroughputWarningCallback(
        OnThroughputWarningCallback callback) = 0;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Reset all statistics and tracking data.
     *
     * Configuration is preserved.
     */
    virtual void reset() = 0;
};

// =============================================================================
// Throughput Optimizer Implementation
// =============================================================================

/**
 * @brief Thread-safe throughput optimizer implementation.
 *
 * Implements IThroughputOptimizer with:
 * - Platform-specific throughput limits (50/500 Mbps desktop, 20/100 Mbps mobile)
 * - Per-stream bitrate tracking with smoothing
 * - Aggregate throughput monitoring
 * - Warning callbacks when limits exceeded
 * - Statistical tracking and reporting
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses std::shared_mutex for read/write locking
 */
class ThroughputOptimizer : public IThroughputOptimizer {
public:
    /**
     * @brief Construct a new ThroughputOptimizer with default configuration.
     */
    ThroughputOptimizer();

    /**
     * @brief Construct a new ThroughputOptimizer with custom configuration.
     *
     * @param config Initial configuration
     */
    explicit ThroughputOptimizer(const ThroughputConfig& config);

    /**
     * @brief Destructor.
     */
    ~ThroughputOptimizer() override = default;

    // Non-copyable
    ThroughputOptimizer(const ThroughputOptimizer&) = delete;
    ThroughputOptimizer& operator=(const ThroughputOptimizer&) = delete;

    // Movable
    ThroughputOptimizer(ThroughputOptimizer&&) noexcept;
    ThroughputOptimizer& operator=(ThroughputOptimizer&&) noexcept;

    // IThroughputOptimizer interface implementation
    void setConfig(const ThroughputConfig& config) override;
    ThroughputConfig getConfig() const override;

    void startStreamTracking(const StreamKey& key) override;
    void stopStreamTracking(const StreamKey& key) override;
    bool isTrackingStream(const StreamKey& key) const override;
    std::vector<StreamKey> getTrackedStreams() const override;

    void recordIngestionData(const StreamKey& key, size_t bytes) override;
    void recordDistributionData(const StreamKey& key, size_t bytes) override;

    std::optional<StreamThroughputStatistics>
    getStreamStatistics(const StreamKey& key) const override;
    AggregateThroughputStatistics getAggregateThroughput() const override;

    bool isIngestionOverLimit(const StreamKey& key) const override;
    bool isDistributionOverLimit(const StreamKey& key) const override;
    bool isAggregateDistributionOverLimit() const override;

    void setOnThroughputWarningCallback(
        OnThroughputWarningCallback callback) override;

    void reset() override;

private:
    /**
     * @brief Data sample for bitrate calculation.
     */
    struct DataSample {
        std::chrono::steady_clock::time_point timestamp;
        size_t bytes;
    };

    /**
     * @brief Internal stream tracking data.
     */
    struct StreamTrackingData {
        StreamKey key;
        std::chrono::steady_clock::time_point trackingStarted;

        // Ingestion data
        uint64_t totalIngestionBytes{0};
        std::deque<DataSample> ingestionSamples;
        double peakIngestionBitrateMbps{0.0};
        std::chrono::steady_clock::time_point lastIngestionWarning;

        // Distribution data
        uint64_t totalDistributionBytes{0};
        std::deque<DataSample> distributionSamples;
        double peakDistributionBitrateMbps{0.0};
        std::chrono::steady_clock::time_point lastDistributionWarning;

        mutable std::mutex mutex;
    };

    /**
     * @brief Calculate current bitrate from samples.
     *
     * @param samples Data samples
     * @param windowMs Smoothing window
     * @return Current bitrate in Mbps
     */
    double calculateCurrentBitrate(
        const std::deque<DataSample>& samples,
        std::chrono::milliseconds windowMs) const;

    /**
     * @brief Clean old samples outside smoothing window.
     *
     * @param samples Data samples to clean
     * @param windowMs Smoothing window
     */
    void cleanOldSamples(
        std::deque<DataSample>& samples,
        std::chrono::milliseconds windowMs);

    /**
     * @brief Calculate average bitrate over entire tracking period.
     *
     * @param totalBytes Total bytes transferred
     * @param trackingStarted When tracking started
     * @return Average bitrate in Mbps
     */
    double calculateAverageBitrate(
        uint64_t totalBytes,
        std::chrono::steady_clock::time_point trackingStarted) const;

    /**
     * @brief Check and invoke warning callback if needed.
     *
     * @param key Stream key
     * @param bitrateMbps Current bitrate
     * @param limitMbps Configured limit
     * @param type Warning type
     * @param lastWarning Reference to last warning time
     */
    void checkAndWarn(
        const StreamKey& key,
        double bitrateMbps,
        double limitMbps,
        ThroughputWarningType type,
        std::chrono::steady_clock::time_point& lastWarning);

    /**
     * @brief Invoke warning callback if set.
     */
    void invokeWarningCallback(
        const StreamKey& key,
        double bitrateMbps,
        ThroughputWarningType type);

    // Configuration
    ThroughputConfig config_;
    mutable std::shared_mutex configMutex_;

    // Stream tracking
    std::map<StreamKey, std::unique_ptr<StreamTrackingData>> streamTracking_;
    mutable std::shared_mutex streamMutex_;

    // Aggregate tracking
    std::deque<DataSample> aggregateIngestionSamples_;
    std::deque<DataSample> aggregateDistributionSamples_;
    uint64_t totalAggregateIngestionBytes_{0};
    uint64_t totalAggregateDistributionBytes_{0};
    std::chrono::steady_clock::time_point lastAggregateWarning_;
    mutable std::mutex aggregateMutex_;

    // Callbacks
    OnThroughputWarningCallback onWarningCallback_;
    mutable std::mutex callbackMutex_;
};

} // namespace streaming
} // namespace openrtmp

#endif // OPENRTMP_STREAMING_THROUGHPUT_OPTIMIZER_HPP
