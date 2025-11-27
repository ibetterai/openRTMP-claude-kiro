// OpenRTMP - Cross-platform RTMP Server
// Publisher Lifecycle Management - Timestamp validation and publisher state management
//
// Responsibilities:
// - Monitor timestamp continuity and log gaps exceeding 1 second
// - Mark stream unavailable within 5 seconds of unexpected publisher disconnect
// - Notify connected subscribers of stream unavailability
// - Track stream statistics including bitrate and frame counts
// - Support graceful unpublish with resource cleanup
//
// Requirements coverage:
// - Requirement 4.5: Mark stream unavailable within 5 seconds of publisher disconnect
// - Requirement 4.7: Validate timestamp continuity, log gaps > 1 second

#ifndef OPENRTMP_STREAMING_PUBLISHER_LIFECYCLE_HPP
#define OPENRTMP_STREAMING_PUBLISHER_LIFECYCLE_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <memory>
#include <mutex>
#include <atomic>
#include <set>
#include <chrono>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/result.hpp"
#include "openrtmp/streaming/media_handler.hpp"

namespace openrtmp {
namespace streaming {

// =============================================================================
// Constants
// =============================================================================

namespace lifecycle {
    /// Timestamp gap threshold in milliseconds (1 second)
    constexpr uint32_t TIMESTAMP_GAP_THRESHOLD_MS = 1000;

    /// Maximum time to mark stream unavailable after disconnect (5 seconds)
    constexpr uint32_t UNAVAILABLE_TIMEOUT_MS = 5000;

    /// Bitrate calculation window in milliseconds
    constexpr uint32_t BITRATE_WINDOW_MS = 2000;
}

// =============================================================================
// Enumerations
// =============================================================================

/**
 * @brief Reason for publisher disconnect.
 */
enum class DisconnectReason {
    Graceful,       ///< Normal unpublish operation
    Unexpected,     ///< Connection lost unexpectedly
    Timeout,        ///< No data received for extended period
    Error           ///< Protocol or other error occurred
};

/**
 * @brief Notification types for subscribers.
 */
enum class StreamNotification {
    StreamStarted,      ///< Stream has started
    StreamEnded,        ///< Stream ended normally
    StreamUnavailable,  ///< Stream became unavailable (unexpected disconnect)
    CodecChanged,       ///< Codec configuration changed
    TimestampGap        ///< Significant timestamp gap detected
};

// =============================================================================
// Data Structures
// =============================================================================

/**
 * @brief Information about a detected timestamp gap.
 */
struct TimestampGapInfo {
    MediaType mediaType;        ///< Type of media with gap
    uint32_t previousTimestamp; ///< Last timestamp before gap
    uint32_t currentTimestamp;  ///< Timestamp after gap
    uint32_t gapMs;             ///< Gap duration in milliseconds

    TimestampGapInfo()
        : mediaType(MediaType::Video)
        , previousTimestamp(0)
        , currentTimestamp(0)
        , gapMs(0)
    {}
};

/**
 * @brief Stream statistics for monitoring and reporting.
 */
struct StreamStatistics {
    uint64_t videoFrameCount;       ///< Total video frames received
    uint64_t audioFrameCount;       ///< Total audio frames received
    uint64_t keyframeCount;         ///< Total keyframes received
    uint64_t totalBytesReceived;    ///< Total bytes received
    uint64_t currentBitrateBps;     ///< Current bitrate in bits per second
    uint64_t durationMs;            ///< Stream duration in milliseconds
    uint32_t timestampGapCount;     ///< Number of timestamp gaps detected
    uint32_t lastVideoTimestamp;    ///< Last video timestamp
    uint32_t lastAudioTimestamp;    ///< Last audio timestamp

    StreamStatistics()
        : videoFrameCount(0)
        , audioFrameCount(0)
        , keyframeCount(0)
        , totalBytesReceived(0)
        , currentBitrateBps(0)
        , durationMs(0)
        , timestampGapCount(0)
        , lastVideoTimestamp(0)
        , lastAudioTimestamp(0)
    {}
};

// =============================================================================
// Callback Types
// =============================================================================

/**
 * @brief Callback for timestamp gap detection.
 * @param gap Information about the detected gap
 */
using TimestampGapCallback = std::function<void(const TimestampGapInfo& gap)>;

/**
 * @brief Callback when stream becomes unavailable.
 */
using StreamUnavailableCallback = std::function<void()>;

/**
 * @brief Callback for publisher disconnect.
 * @param reason The reason for disconnect
 */
using DisconnectCallback = std::function<void(DisconnectReason reason)>;

/**
 * @brief Callback to notify individual subscribers.
 * @param subscriberId The subscriber to notify
 * @param notification The notification type
 */
using SubscriberNotificationCallback = std::function<void(
    SubscriberId subscriberId,
    StreamNotification notification
)>;

// =============================================================================
// Publisher Lifecycle Interface
// =============================================================================

/**
 * @brief Interface for publisher lifecycle management.
 *
 * Defines the contract for managing publisher state, timestamp validation,
 * and subscriber notifications.
 */
class IPublisherLifecycle {
public:
    virtual ~IPublisherLifecycle() = default;

    // -------------------------------------------------------------------------
    // Lifecycle Control
    // -------------------------------------------------------------------------

    /**
     * @brief Start the publisher lifecycle manager.
     *
     * Initializes state and begins monitoring.
     */
    virtual void start() = 0;

    /**
     * @brief Stop the publisher lifecycle manager.
     *
     * Performs cleanup and notifies subscribers of stream end.
     */
    virtual void stop() = 0;

    /**
     * @brief Check if lifecycle manager is active.
     * @return true if active
     */
    virtual bool isActive() const = 0;

    // -------------------------------------------------------------------------
    // Media Processing
    // -------------------------------------------------------------------------

    /**
     * @brief Process received media message.
     *
     * Updates statistics and checks timestamp continuity.
     *
     * @param msg The media message to process
     */
    virtual void onMediaReceived(const MediaMessage& msg) = 0;

    // -------------------------------------------------------------------------
    // Publisher Disconnect Handling
    // -------------------------------------------------------------------------

    /**
     * @brief Handle publisher disconnect event.
     *
     * For unexpected disconnects, triggers 5-second unavailability notification.
     *
     * @param reason The reason for disconnect
     */
    virtual void onPublisherDisconnected(DisconnectReason reason) = 0;

    // -------------------------------------------------------------------------
    // Subscriber Management
    // -------------------------------------------------------------------------

    /**
     * @brief Add a subscriber to track.
     * @param subscriberId The subscriber ID
     */
    virtual void addSubscriber(SubscriberId subscriberId) = 0;

    /**
     * @brief Remove a tracked subscriber.
     * @param subscriberId The subscriber ID
     */
    virtual void removeSubscriber(SubscriberId subscriberId) = 0;

    /**
     * @brief Get current subscriber count.
     * @return Number of tracked subscribers
     */
    virtual size_t getSubscriberCount() const = 0;

    // -------------------------------------------------------------------------
    // Statistics
    // -------------------------------------------------------------------------

    /**
     * @brief Get current stream statistics.
     * @return Stream statistics structure
     */
    virtual StreamStatistics getStatistics() const = 0;

    // -------------------------------------------------------------------------
    // Callbacks
    // -------------------------------------------------------------------------

    /**
     * @brief Set callback for timestamp gap detection.
     * @param callback The callback to invoke
     */
    virtual void setTimestampGapCallback(TimestampGapCallback callback) = 0;

    /**
     * @brief Set callback for stream unavailability.
     * @param callback The callback to invoke
     */
    virtual void setStreamUnavailableCallback(StreamUnavailableCallback callback) = 0;

    /**
     * @brief Set callback for publisher disconnect.
     * @param callback The callback to invoke
     */
    virtual void setDisconnectCallback(DisconnectCallback callback) = 0;

    /**
     * @brief Set callback for subscriber notifications.
     * @param callback The callback to invoke
     */
    virtual void setSubscriberNotificationCallback(SubscriberNotificationCallback callback) = 0;

    /**
     * @brief Set custom unavailability timeout (for testing).
     * @param timeoutMs Timeout in milliseconds
     */
    virtual void setUnavailabilityTimeout(uint32_t timeoutMs) = 0;
};

// =============================================================================
// Publisher Lifecycle Implementation
// =============================================================================

/**
 * @brief Thread-safe publisher lifecycle manager implementation.
 *
 * Implements IPublisherLifecycle with:
 * - Timestamp continuity monitoring with gap detection (> 1 second)
 * - Publisher disconnect handling with 5-second unavailability notification
 * - Subscriber tracking and notification
 * - Stream statistics collection
 * - Graceful unpublish with cleanup
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses mutex for state protection
 * - Background thread for unavailability timeout
 */
class PublisherLifecycle : public IPublisherLifecycle {
public:
    /**
     * @brief Construct a new PublisherLifecycle.
     */
    PublisherLifecycle();

    /**
     * @brief Destructor.
     */
    ~PublisherLifecycle() override;

    // Non-copyable
    PublisherLifecycle(const PublisherLifecycle&) = delete;
    PublisherLifecycle& operator=(const PublisherLifecycle&) = delete;

    // Movable
    PublisherLifecycle(PublisherLifecycle&&) noexcept;
    PublisherLifecycle& operator=(PublisherLifecycle&&) noexcept;

    // IPublisherLifecycle interface implementation
    void start() override;
    void stop() override;
    bool isActive() const override;

    void onMediaReceived(const MediaMessage& msg) override;
    void onPublisherDisconnected(DisconnectReason reason) override;

    void addSubscriber(SubscriberId subscriberId) override;
    void removeSubscriber(SubscriberId subscriberId) override;
    size_t getSubscriberCount() const override;

    StreamStatistics getStatistics() const override;

    void setTimestampGapCallback(TimestampGapCallback callback) override;
    void setStreamUnavailableCallback(StreamUnavailableCallback callback) override;
    void setDisconnectCallback(DisconnectCallback callback) override;
    void setSubscriberNotificationCallback(SubscriberNotificationCallback callback) override;
    void setUnavailabilityTimeout(uint32_t timeoutMs) override;

private:
    // -------------------------------------------------------------------------
    // Internal Methods
    // -------------------------------------------------------------------------

    /**
     * @brief Check for timestamp gap and invoke callback if needed.
     */
    void checkTimestampGap(MediaType type, uint32_t timestamp);

    /**
     * @brief Update bitrate calculation.
     */
    void updateBitrate(size_t bytes);

    /**
     * @brief Notify all subscribers of a notification type.
     */
    void notifyAllSubscribers(StreamNotification notification);

    /**
     * @brief Reset internal state.
     */
    void resetState();

    /**
     * @brief Background thread function for unavailability timeout.
     */
    void unavailabilityTimeoutThread();

    /**
     * @brief Handle timestamp wraparound detection.
     */
    bool isTimestampWraparound(uint32_t previous, uint32_t current) const;

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    std::atomic<bool> active_;
    std::atomic<bool> disconnected_;
    std::atomic<bool> disconnectNotified_;

    // Statistics
    StreamStatistics stats_;

    // Timestamp tracking
    bool hasFirstVideoTimestamp_;
    bool hasFirstAudioTimestamp_;

    // Subscriber tracking
    std::set<SubscriberId> subscribers_;

    // Bitrate calculation
    std::chrono::steady_clock::time_point startTime_;
    std::chrono::steady_clock::time_point lastBitrateCalcTime_;
    uint64_t bytesInWindow_;

    // Thread safety
    mutable std::mutex mutex_;
    mutable std::mutex callbackMutex_;

    // Callbacks
    TimestampGapCallback timestampGapCallback_;
    StreamUnavailableCallback streamUnavailableCallback_;
    DisconnectCallback disconnectCallback_;
    SubscriberNotificationCallback subscriberNotificationCallback_;

    // Configurable timeout (default 5 seconds - for documentation/future async support)
    uint32_t unavailabilityTimeoutMs_;
};

} // namespace streaming
} // namespace openrtmp

#endif // OPENRTMP_STREAMING_PUBLISHER_LIFECYCLE_HPP
