// OpenRTMP - Cross-platform RTMP Server
// Subscriber Buffer - Per-subscriber buffer management with overflow protection
//
// Responsibilities:
// - Track per-subscriber buffer levels independently
// - Drop non-keyframe packets when buffer exceeds configured threshold
// - Preserve keyframes and audio to maintain stream continuity
// - Log dropped frame statistics per subscriber
// - Support configurable maximum buffer thresholds
//
// Requirements coverage:
// - Requirement 5.4: Maintain independent send buffers for each subscriber
// - Requirement 5.5: Drop non-keyframe packets if buffer exceeds 5 seconds

#ifndef OPENRTMP_STREAMING_SUBSCRIBER_BUFFER_HPP
#define OPENRTMP_STREAMING_SUBSCRIBER_BUFFER_HPP

#include <cstdint>
#include <vector>
#include <optional>
#include <chrono>
#include <memory>
#include <mutex>
#include <deque>
#include <map>
#include <functional>

#include "openrtmp/core/types.hpp"
#include "openrtmp/streaming/gop_buffer.hpp"

namespace openrtmp {
namespace streaming {

// =============================================================================
// Data Structures
// =============================================================================

/**
 * @brief Configuration for per-subscriber buffer behavior.
 */
struct SubscriberBufferConfig {
    /// Maximum buffer duration before dropping frames (default 5 seconds per req 5.5)
    std::chrono::milliseconds maxBufferDuration{5000};

    /// Maximum buffer size in bytes (0 = unlimited)
    size_t maxBufferSize{0};

    /// Whether to preserve audio frames when dropping (default true)
    bool preserveAudio{true};

    /// Whether to preserve keyframes when dropping (always true, cannot be disabled)
    /// This is informational only - keyframes are always preserved
    bool preserveKeyframes{true};

    SubscriberBufferConfig() = default;
};

/**
 * @brief Statistics about dropped frames per subscriber.
 */
struct SubscriberBufferStatistics {
    /// Number of video inter frames (P/B frames) dropped
    uint64_t droppedInterFrames{0};

    /// Number of keyframes dropped (should always be 0)
    uint64_t droppedKeyframes{0};

    /// Number of audio frames dropped
    uint64_t droppedAudioFrames{0};

    /// Total bytes of dropped frames
    uint64_t totalDroppedBytes{0};

    /// Time of last frame drop
    std::optional<std::chrono::steady_clock::time_point> lastDropTime;

    /// Total frames processed
    uint64_t totalFramesProcessed{0};

    /// Total bytes processed
    uint64_t totalBytesProcessed{0};
};

/**
 * @brief Subscriber statistics for monitoring.
 */
struct SubscriberStats {
    SubscriberId subscriberId{0};
    uint64_t pendingFrames{0};
    uint64_t pendingBytes{0};
    std::chrono::milliseconds bufferLevel{0};
    uint64_t droppedFrames{0};
    uint64_t deliveredFrames{0};
};

/**
 * @brief Callback type for frame drop notifications.
 */
using OnFrameDroppedCallback = std::function<void(const BufferedFrame& frame)>;

// =============================================================================
// Subscriber Buffer Interface
// =============================================================================

/**
 * @brief Interface for per-subscriber buffer operations.
 *
 * Each subscriber has an independent buffer to prevent slow clients
 * from affecting other subscribers. When the buffer exceeds the
 * configured threshold (default 5 seconds), non-keyframe video
 * packets are dropped to prevent unbounded memory growth.
 */
class ISubscriberBuffer {
public:
    virtual ~ISubscriberBuffer() = default;

    // -------------------------------------------------------------------------
    // Frame Management
    // -------------------------------------------------------------------------

    /**
     * @brief Push a frame into the subscriber's buffer.
     *
     * If the buffer exceeds maxBufferDuration, non-keyframe video
     * frames will be dropped. Keyframes and audio (if configured)
     * are always preserved.
     *
     * @param frame The frame to add
     */
    virtual void push(const BufferedFrame& frame) = 0;

    /**
     * @brief Pop the next frame from the buffer.
     *
     * @return The next frame, or nullopt if buffer is empty
     */
    virtual std::optional<BufferedFrame> pop() = 0;

    /**
     * @brief Peek at the next frame without removing it.
     *
     * @return The next frame, or nullopt if buffer is empty
     */
    virtual std::optional<BufferedFrame> peek() const = 0;

    // -------------------------------------------------------------------------
    // Buffer Status
    // -------------------------------------------------------------------------

    /**
     * @brief Get the current buffer level (duration from oldest to newest frame).
     *
     * @return Buffer duration in milliseconds
     */
    virtual std::chrono::milliseconds getBufferLevel() const = 0;

    /**
     * @brief Get total bytes currently buffered.
     *
     * @return Size in bytes
     */
    virtual size_t getBufferedBytes() const = 0;

    /**
     * @brief Get number of frames pending in buffer.
     *
     * @return Frame count
     */
    virtual size_t getPendingFrameCount() const = 0;

    /**
     * @brief Check if buffer is empty.
     *
     * @return true if no frames in buffer
     */
    virtual bool isEmpty() const = 0;

    // -------------------------------------------------------------------------
    // Statistics
    // -------------------------------------------------------------------------

    /**
     * @brief Get count of dropped frames since last reset.
     *
     * @return Number of dropped frames
     */
    virtual uint64_t getDroppedFrameCount() const = 0;

    /**
     * @brief Get detailed buffer statistics.
     *
     * @return Statistics structure
     */
    virtual SubscriberBufferStatistics getStatistics() const = 0;

    /**
     * @brief Reset statistics counters.
     */
    virtual void resetStatistics() = 0;

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    /**
     * @brief Set buffer configuration.
     *
     * @param config New configuration
     */
    virtual void setConfig(const SubscriberBufferConfig& config) = 0;

    /**
     * @brief Get current configuration.
     *
     * @return Current configuration
     */
    virtual SubscriberBufferConfig getConfig() const = 0;

    // -------------------------------------------------------------------------
    // Callbacks
    // -------------------------------------------------------------------------

    /**
     * @brief Set callback invoked when frames are dropped.
     *
     * @param callback Callback function
     */
    virtual void setOnFrameDroppedCallback(OnFrameDroppedCallback callback) = 0;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Clear all frames but preserve statistics.
     */
    virtual void clear() = 0;

    /**
     * @brief Reset to initial state, clearing frames and statistics.
     */
    virtual void reset() = 0;
};

// =============================================================================
// Subscriber Buffer Implementation
// =============================================================================

/**
 * @brief Thread-safe per-subscriber buffer with overflow protection.
 *
 * Implements ISubscriberBuffer with:
 * - Automatic frame dropping when buffer exceeds threshold
 * - Keyframe and audio preservation during dropping
 * - Detailed statistics tracking
 * - Thread-safe access using mutex protection
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses std::mutex for read/write synchronization
 */
class SubscriberBuffer : public ISubscriberBuffer {
public:
    /**
     * @brief Construct a new SubscriberBuffer with default configuration.
     */
    SubscriberBuffer();

    /**
     * @brief Construct a new SubscriberBuffer with custom configuration.
     *
     * @param config Buffer configuration
     */
    explicit SubscriberBuffer(const SubscriberBufferConfig& config);

    /**
     * @brief Destructor.
     */
    ~SubscriberBuffer() override = default;

    // Non-copyable
    SubscriberBuffer(const SubscriberBuffer&) = delete;
    SubscriberBuffer& operator=(const SubscriberBuffer&) = delete;

    // Movable
    SubscriberBuffer(SubscriberBuffer&&) noexcept;
    SubscriberBuffer& operator=(SubscriberBuffer&&) noexcept;

    // ISubscriberBuffer interface implementation
    void push(const BufferedFrame& frame) override;
    std::optional<BufferedFrame> pop() override;
    std::optional<BufferedFrame> peek() const override;

    std::chrono::milliseconds getBufferLevel() const override;
    size_t getBufferedBytes() const override;
    size_t getPendingFrameCount() const override;
    bool isEmpty() const override;

    uint64_t getDroppedFrameCount() const override;
    SubscriberBufferStatistics getStatistics() const override;
    void resetStatistics() override;

    void setConfig(const SubscriberBufferConfig& config) override;
    SubscriberBufferConfig getConfig() const override;

    void setOnFrameDroppedCallback(OnFrameDroppedCallback callback) override;

    void clear() override;
    void reset() override;

private:
    /**
     * @brief Check if frame should be dropped based on current buffer state.
     *
     * @param frame The frame to evaluate
     * @return true if frame should be dropped
     */
    bool shouldDropFrame(const BufferedFrame& frame) const;

    /**
     * @brief Drop frames to bring buffer within threshold.
     *
     * Drops non-keyframe video frames while preserving keyframes and audio.
     */
    void enforceBufferLimit();

    /**
     * @brief Calculate buffer duration from timestamps.
     *
     * @return Duration in milliseconds
     */
    std::chrono::milliseconds calculateBufferDuration() const;

    /**
     * @brief Record a dropped frame in statistics.
     *
     * @param frame The dropped frame
     */
    void recordDroppedFrame(const BufferedFrame& frame);

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    // Frame storage
    std::deque<BufferedFrame> frames_;

    // Statistics
    size_t bufferedBytes_{0};
    SubscriberBufferStatistics statistics_;

    // Configuration
    SubscriberBufferConfig config_;

    // Callbacks
    OnFrameDroppedCallback onFrameDroppedCallback_;

    // Thread safety
    mutable std::mutex mutex_;
};

// =============================================================================
// Subscriber Buffer Manager
// =============================================================================

/**
 * @brief Manages multiple subscriber buffers independently.
 *
 * Implements requirement 5.4: Maintain independent send buffers for
 * each subscriber to prevent slow clients from affecting others.
 */
class SubscriberBufferManager {
public:
    SubscriberBufferManager() = default;
    ~SubscriberBufferManager() = default;

    // Non-copyable
    SubscriberBufferManager(const SubscriberBufferManager&) = delete;
    SubscriberBufferManager& operator=(const SubscriberBufferManager&) = delete;

    // -------------------------------------------------------------------------
    // Subscriber Management
    // -------------------------------------------------------------------------

    /**
     * @brief Add a new subscriber with default configuration.
     *
     * @param subscriberId Unique subscriber identifier
     */
    void addSubscriber(SubscriberId subscriberId);

    /**
     * @brief Add a new subscriber with custom configuration.
     *
     * @param subscriberId Unique subscriber identifier
     * @param config Buffer configuration for this subscriber
     */
    void addSubscriber(SubscriberId subscriberId, const SubscriberBufferConfig& config);

    /**
     * @brief Remove a subscriber and clean up their buffer.
     *
     * @param subscriberId Subscriber to remove
     */
    void removeSubscriber(SubscriberId subscriberId);

    /**
     * @brief Check if a subscriber exists.
     *
     * @param subscriberId Subscriber to check
     * @return true if subscriber exists
     */
    bool hasSubscriber(SubscriberId subscriberId) const;

    /**
     * @brief Get list of all subscriber IDs.
     *
     * @return Vector of subscriber IDs
     */
    std::vector<SubscriberId> getSubscriberIds() const;

    /**
     * @brief Get count of active subscribers.
     *
     * @return Number of subscribers
     */
    size_t getSubscriberCount() const;

    // -------------------------------------------------------------------------
    // Frame Distribution
    // -------------------------------------------------------------------------

    /**
     * @brief Push a frame to a specific subscriber's buffer.
     *
     * @param subscriberId Target subscriber
     * @param frame Frame to push
     */
    void push(SubscriberId subscriberId, const BufferedFrame& frame);

    /**
     * @brief Distribute a frame to all subscribers.
     *
     * Each subscriber's buffer handles overflow independently.
     *
     * @param frame Frame to distribute
     */
    void distributeToAll(const BufferedFrame& frame);

    /**
     * @brief Pop a frame from a specific subscriber's buffer.
     *
     * @param subscriberId Target subscriber
     * @return Frame or nullopt if empty
     */
    std::optional<BufferedFrame> pop(SubscriberId subscriberId);

    // -------------------------------------------------------------------------
    // Statistics
    // -------------------------------------------------------------------------

    /**
     * @brief Get statistics for a specific subscriber.
     *
     * @param subscriberId Target subscriber
     * @return Subscriber statistics
     */
    SubscriberStats getSubscriberStats(SubscriberId subscriberId) const;

    /**
     * @brief Get statistics for all subscribers.
     *
     * @return Vector of all subscriber statistics
     */
    std::vector<SubscriberStats> getAllSubscriberStats() const;

    /**
     * @brief Get total dropped frames across all subscribers.
     *
     * @return Total dropped frame count
     */
    uint64_t getTotalDroppedFrames() const;

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    /**
     * @brief Set configuration for a specific subscriber.
     *
     * @param subscriberId Target subscriber
     * @param config New configuration
     */
    void setSubscriberConfig(SubscriberId subscriberId, const SubscriberBufferConfig& config);

    /**
     * @brief Get configuration for a specific subscriber.
     *
     * @param subscriberId Target subscriber
     * @return Subscriber's configuration
     */
    SubscriberBufferConfig getSubscriberConfig(SubscriberId subscriberId) const;

    /**
     * @brief Set default configuration for new subscribers.
     *
     * @param config Default configuration
     */
    void setDefaultConfig(const SubscriberBufferConfig& config);

    // -------------------------------------------------------------------------
    // Callbacks
    // -------------------------------------------------------------------------

    /**
     * @brief Set callback for frame drops across all subscribers.
     *
     * @param callback Callback function receiving subscriber ID and dropped frame
     */
    void setOnFrameDroppedCallback(
        std::function<void(SubscriberId, const BufferedFrame&)> callback
    );

private:
    // Per-subscriber buffers
    std::map<SubscriberId, std::unique_ptr<SubscriberBuffer>> buffers_;

    // Default configuration for new subscribers
    SubscriberBufferConfig defaultConfig_;

    // Global callback
    std::function<void(SubscriberId, const BufferedFrame&)> onFrameDroppedCallback_;

    // Thread safety
    mutable std::mutex mutex_;
};

} // namespace streaming
} // namespace openrtmp

#endif // OPENRTMP_STREAMING_SUBSCRIBER_BUFFER_HPP
