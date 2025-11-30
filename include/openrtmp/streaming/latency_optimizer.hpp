// OpenRTMP - Cross-platform RTMP Server
// Latency Optimizer - Latency measurement, tracking, and optimization
//
// Responsibilities:
// - Target glass-to-glass latency under 2 seconds on desktop
// - Target glass-to-glass latency under 3 seconds on mobile
// - Process and forward chunks within 50ms of receipt
// - Support low-latency mode with 500ms maximum subscriber buffer
// - Latency measurement and tracking
// - Latency statistics and reporting
//
// Requirements coverage:
// - Requirement 12.1: Glass-to-glass latency <2s on desktop
// - Requirement 12.2: Glass-to-glass latency <3s on mobile
// - Requirement 12.3: Process and forward chunks within 50ms
// - Requirement 12.4: Low-latency mode option
// - Requirement 12.5: Low-latency mode 500ms maximum buffer

#ifndef OPENRTMP_STREAMING_LATENCY_OPTIMIZER_HPP
#define OPENRTMP_STREAMING_LATENCY_OPTIMIZER_HPP

#include <cstdint>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace streaming {

// =============================================================================
// Configuration Types
// =============================================================================

/**
 * @brief Configuration for latency optimization behavior.
 */
struct LatencyConfig {
    /// Target glass-to-glass latency (default 2s for desktop per req 12.1)
    std::chrono::milliseconds targetGlassToGlassLatency{2000};

    /// Maximum chunk processing time (default 50ms per req 12.3)
    std::chrono::milliseconds maxProcessingTime{50};

    /// Enable low-latency mode (req 12.4)
    bool lowLatencyMode{false};

    /// Maximum subscriber buffer in low-latency mode (500ms per req 12.5)
    std::chrono::milliseconds maxSubscriberBuffer{5000};

    /// Normal mode buffer size (default 5 seconds)
    std::chrono::milliseconds normalModeBuffer{5000};

    /// Latency alert threshold (triggers callback when exceeded)
    std::chrono::milliseconds alertThreshold{1500};

    LatencyConfig() = default;

    /**
     * @brief Create configuration for desktop platforms.
     *
     * Desktop target: <2 seconds glass-to-glass (Requirement 12.1)
     *
     * @return Desktop-optimized configuration
     */
    static LatencyConfig forDesktop() {
        LatencyConfig config;
        config.targetGlassToGlassLatency = std::chrono::milliseconds(2000);
        config.maxProcessingTime = std::chrono::milliseconds(50);
        config.alertThreshold = std::chrono::milliseconds(1500);
        return config;
    }

    /**
     * @brief Create configuration for mobile platforms.
     *
     * Mobile target: <3 seconds glass-to-glass (Requirement 12.2)
     *
     * @return Mobile-optimized configuration
     */
    static LatencyConfig forMobile() {
        LatencyConfig config;
        config.targetGlassToGlassLatency = std::chrono::milliseconds(3000);
        config.maxProcessingTime = std::chrono::milliseconds(50);
        config.alertThreshold = std::chrono::milliseconds(2500);
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
    static LatencyConfig forCurrentPlatform() {
        if (core::isDesktopPlatform()) {
            return forDesktop();
        } else if (core::isMobilePlatform()) {
            return forMobile();
        }
        // Default to desktop if unknown
        return forDesktop();
    }

    /**
     * @brief Create low-latency configuration.
     *
     * Enables low-latency mode with 500ms max buffer (Requirements 12.4, 12.5)
     *
     * @return Low-latency configuration
     */
    static LatencyConfig lowLatency() {
        LatencyConfig config = forCurrentPlatform();
        config.lowLatencyMode = true;
        config.maxSubscriberBuffer = std::chrono::milliseconds(500);
        config.alertThreshold = std::chrono::milliseconds(400);
        return config;
    }
};

// =============================================================================
// Statistics Types
// =============================================================================

/**
 * @brief Statistics for chunk processing performance.
 */
struct ProcessingStatistics {
    /// Total number of chunks processed
    uint64_t totalChunksProcessed{0};

    /// Number of chunks that exceeded processing time target
    uint64_t slowChunksCount{0};

    /// Average chunk processing time
    std::chrono::milliseconds averageProcessingTime{0};

    /// Minimum processing time observed
    std::chrono::milliseconds minProcessingTime{0};

    /// Maximum processing time observed
    std::chrono::milliseconds maxProcessingTime{0};

    /// Percentage of chunks processed within target time
    double onTimePercentage{100.0};
};

/**
 * @brief Latency statistics for a stream.
 */
struct StreamLatencyStatistics {
    /// Stream key
    StreamKey streamKey;

    /// Number of latency samples
    uint64_t sampleCount{0};

    /// Average latency across all samples
    std::chrono::milliseconds averageLatency{0};

    /// Minimum latency observed
    std::chrono::milliseconds minLatency{0};

    /// Maximum latency observed
    std::chrono::milliseconds maxLatency{0};

    /// 50th percentile latency
    std::chrono::milliseconds p50Latency{0};

    /// 95th percentile latency
    std::chrono::milliseconds p95Latency{0};

    /// 99th percentile latency
    std::chrono::milliseconds p99Latency{0};

    /// Time when tracking started
    std::chrono::steady_clock::time_point trackingStarted;

    /// Whether currently within target latency
    bool withinTarget{true};
};

// =============================================================================
// Callback Types
// =============================================================================

/**
 * @brief Callback invoked when latency exceeds threshold.
 *
 * @param key Stream key with high latency
 * @param latency The recorded latency value
 */
using OnHighLatencyCallback = std::function<void(
    const StreamKey& key,
    std::chrono::milliseconds latency
)>;

/**
 * @brief Callback invoked when chunk processing exceeds target time.
 *
 * @param chunkId The chunk that was slow to process
 * @param processingTime Time taken to process the chunk
 */
using OnSlowProcessingCallback = std::function<void(
    uint32_t chunkId,
    std::chrono::milliseconds processingTime
)>;

// =============================================================================
// Latency Optimizer Interface
// =============================================================================

/**
 * @brief Interface for latency optimization operations.
 */
class ILatencyOptimizer {
public:
    virtual ~ILatencyOptimizer() = default;

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    /**
     * @brief Set latency configuration.
     *
     * @param config New configuration
     */
    virtual void setConfig(const LatencyConfig& config) = 0;

    /**
     * @brief Get current configuration.
     *
     * @return Current configuration
     */
    virtual LatencyConfig getConfig() const = 0;

    /**
     * @brief Enable low-latency mode.
     *
     * Activates low-latency mode with 500ms max buffer per requirement 12.5.
     */
    virtual void enableLowLatencyMode() = 0;

    /**
     * @brief Disable low-latency mode.
     *
     * Returns to normal buffering mode.
     */
    virtual void disableLowLatencyMode() = 0;

    // -------------------------------------------------------------------------
    // Chunk Processing Tracking (Requirement 12.3)
    // -------------------------------------------------------------------------

    /**
     * @brief Record when a chunk is received.
     *
     * @param chunkId Unique identifier for the chunk
     */
    virtual void recordChunkReceived(uint32_t chunkId) = 0;

    /**
     * @brief Record when a chunk is forwarded to subscribers.
     *
     * @param chunkId Unique identifier for the chunk
     */
    virtual void recordChunkForwarded(uint32_t chunkId) = 0;

    /**
     * @brief Get the receive time for a chunk.
     *
     * @param chunkId Chunk identifier
     * @return Receive time or empty if not found
     */
    virtual std::optional<std::chrono::steady_clock::time_point>
    getChunkReceiveTime(uint32_t chunkId) const = 0;

    /**
     * @brief Get processing time for a chunk.
     *
     * @param chunkId Chunk identifier
     * @return Processing time or empty if not tracked
     */
    virtual std::optional<std::chrono::milliseconds>
    getChunkProcessingTime(uint32_t chunkId) const = 0;

    /**
     * @brief Check if chunk processing was slow.
     *
     * @param chunkId Chunk identifier
     * @return true if processing exceeded target time
     */
    virtual bool wasProcessingTimeSlow(uint32_t chunkId) const = 0;

    // -------------------------------------------------------------------------
    // Stream Latency Tracking
    // -------------------------------------------------------------------------

    /**
     * @brief Start tracking latency for a stream.
     *
     * @param key Stream key
     */
    virtual void startStreamTracking(const StreamKey& key) = 0;

    /**
     * @brief Stop tracking latency for a stream.
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

    /**
     * @brief Record frame latency for a stream.
     *
     * @param key Stream key
     * @param latency End-to-end latency for the frame
     */
    virtual void recordFrameLatency(
        const StreamKey& key,
        std::chrono::milliseconds latency
    ) = 0;

    /**
     * @brief Get latency statistics for a stream.
     *
     * @param key Stream key
     * @return Latency statistics or empty if not tracked
     */
    virtual std::optional<StreamLatencyStatistics>
    getStreamLatency(const StreamKey& key) const = 0;

    /**
     * @brief Check if stream latency is within target.
     *
     * @param key Stream key
     * @return true if latency is within configured target
     */
    virtual bool isWithinLatencyTarget(const StreamKey& key) const = 0;

    // -------------------------------------------------------------------------
    // Statistics
    // -------------------------------------------------------------------------

    /**
     * @brief Get processing statistics.
     *
     * @return Processing statistics
     */
    virtual ProcessingStatistics getProcessingStatistics() const = 0;

    /**
     * @brief Get recommended buffer size based on current mode.
     *
     * @return Recommended buffer duration
     */
    virtual std::chrono::milliseconds getRecommendedBufferSize() const = 0;

    // -------------------------------------------------------------------------
    // Callbacks
    // -------------------------------------------------------------------------

    /**
     * @brief Set callback for high latency events.
     *
     * @param callback Callback function
     */
    virtual void setOnHighLatencyCallback(OnHighLatencyCallback callback) = 0;

    /**
     * @brief Set callback for slow processing events.
     *
     * @param callback Callback function
     */
    virtual void setOnSlowProcessingCallback(OnSlowProcessingCallback callback) = 0;

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
// Latency Optimizer Implementation
// =============================================================================

/**
 * @brief Thread-safe latency optimizer implementation.
 *
 * Implements ILatencyOptimizer with:
 * - Platform-specific latency targets (2s desktop, 3s mobile)
 * - Chunk processing time tracking with 50ms target
 * - Low-latency mode support (500ms buffer)
 * - Statistical tracking and percentile calculations
 * - High latency and slow processing alerts
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses std::shared_mutex for read/write locking
 */
class LatencyOptimizer : public ILatencyOptimizer {
public:
    /**
     * @brief Construct a new LatencyOptimizer with default configuration.
     */
    LatencyOptimizer();

    /**
     * @brief Construct a new LatencyOptimizer with custom configuration.
     *
     * @param config Initial configuration
     */
    explicit LatencyOptimizer(const LatencyConfig& config);

    /**
     * @brief Destructor.
     */
    ~LatencyOptimizer() override = default;

    // Non-copyable
    LatencyOptimizer(const LatencyOptimizer&) = delete;
    LatencyOptimizer& operator=(const LatencyOptimizer&) = delete;

    // Movable
    LatencyOptimizer(LatencyOptimizer&&) noexcept;
    LatencyOptimizer& operator=(LatencyOptimizer&&) noexcept;

    // ILatencyOptimizer interface implementation
    void setConfig(const LatencyConfig& config) override;
    LatencyConfig getConfig() const override;
    void enableLowLatencyMode() override;
    void disableLowLatencyMode() override;

    void recordChunkReceived(uint32_t chunkId) override;
    void recordChunkForwarded(uint32_t chunkId) override;
    std::optional<std::chrono::steady_clock::time_point>
    getChunkReceiveTime(uint32_t chunkId) const override;
    std::optional<std::chrono::milliseconds>
    getChunkProcessingTime(uint32_t chunkId) const override;
    bool wasProcessingTimeSlow(uint32_t chunkId) const override;

    void startStreamTracking(const StreamKey& key) override;
    void stopStreamTracking(const StreamKey& key) override;
    bool isTrackingStream(const StreamKey& key) const override;
    std::vector<StreamKey> getTrackedStreams() const override;
    void recordFrameLatency(
        const StreamKey& key,
        std::chrono::milliseconds latency
    ) override;
    std::optional<StreamLatencyStatistics>
    getStreamLatency(const StreamKey& key) const override;
    bool isWithinLatencyTarget(const StreamKey& key) const override;

    ProcessingStatistics getProcessingStatistics() const override;
    std::chrono::milliseconds getRecommendedBufferSize() const override;

    void setOnHighLatencyCallback(OnHighLatencyCallback callback) override;
    void setOnSlowProcessingCallback(OnSlowProcessingCallback callback) override;

    void reset() override;

private:
    /**
     * @brief Internal chunk tracking data.
     */
    struct ChunkTracking {
        std::chrono::steady_clock::time_point receiveTime;
        std::optional<std::chrono::steady_clock::time_point> forwardTime;
        bool slow{false};
    };

    /**
     * @brief Internal stream latency tracking data.
     */
    struct StreamTrackingData {
        StreamKey key;
        std::chrono::steady_clock::time_point trackingStarted;
        std::vector<std::chrono::milliseconds> latencySamples;
        mutable std::mutex mutex;
    };

    /**
     * @brief Calculate percentile from sorted samples.
     */
    std::chrono::milliseconds calculatePercentile(
        const std::vector<std::chrono::milliseconds>& sortedSamples,
        double percentile
    ) const;

    /**
     * @brief Update processing statistics after chunk forward.
     */
    void updateProcessingStats(std::chrono::milliseconds processingTime,
                                bool wasSlow);

    /**
     * @brief Invoke high latency callback if set.
     */
    void invokeHighLatencyCallback(const StreamKey& key,
                                    std::chrono::milliseconds latency);

    /**
     * @brief Invoke slow processing callback if set.
     */
    void invokeSlowProcessingCallback(uint32_t chunkId,
                                       std::chrono::milliseconds time);

    // Configuration
    LatencyConfig config_;
    mutable std::shared_mutex configMutex_;

    // Chunk tracking
    std::unordered_map<uint32_t, ChunkTracking> chunkTracking_;
    mutable std::shared_mutex chunkMutex_;

    // Processing statistics
    uint64_t totalChunksProcessed_{0};
    uint64_t slowChunksCount_{0};
    std::chrono::milliseconds totalProcessingTime_{0};
    std::chrono::milliseconds minProcessingTime_{std::chrono::milliseconds::max()};
    std::chrono::milliseconds maxProcessingTime_{0};
    mutable std::mutex procStatsMutex_;

    // Stream latency tracking
    std::map<StreamKey, std::unique_ptr<StreamTrackingData>> streamTracking_;
    mutable std::shared_mutex streamMutex_;

    // Callbacks
    OnHighLatencyCallback onHighLatencyCallback_;
    OnSlowProcessingCallback onSlowProcessingCallback_;
    mutable std::mutex callbackMutex_;
};

} // namespace streaming
} // namespace openrtmp

#endif // OPENRTMP_STREAMING_LATENCY_OPTIMIZER_HPP
