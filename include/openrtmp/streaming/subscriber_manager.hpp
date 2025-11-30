// OpenRTMP - Cross-platform RTMP Server
// Subscriber Manager - Stream subscription management
//
// Responsibilities:
// - Add subscribers to active streams with configuration options
// - Maintain independent send buffers per subscriber
// - Support low-latency mode with 500ms maximum buffer
// - Remove subscribers cleanly on disconnect or stop
// - Track subscriber statistics including bytes delivered and dropped frames
//
// Requirements coverage:
// - Requirement 5.2: Support multiple simultaneous subscribers for a single stream
// - Requirement 5.4: Maintain independent send buffers for each subscriber
// - Requirement 12.4: Low-latency mode option reducing buffering
// - Requirement 12.5: Low-latency mode 500ms maximum buffer per subscriber

#ifndef OPENRTMP_STREAMING_SUBSCRIBER_MANAGER_HPP
#define OPENRTMP_STREAMING_SUBSCRIBER_MANAGER_HPP

#include <cstdint>
#include <string>
#include <optional>
#include <memory>
#include <functional>
#include <set>
#include <map>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <chrono>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/result.hpp"
#include "openrtmp/streaming/subscriber_buffer.hpp"
#include "openrtmp/streaming/gop_buffer.hpp"

namespace openrtmp {
namespace streaming {

// Forward declarations
class IStreamRegistry;
class StreamRegistry;

// =============================================================================
// Configuration Types
// =============================================================================

/**
 * @brief Configuration for a subscriber.
 *
 * Allows customization of buffer behavior per subscriber,
 * including low-latency mode settings.
 */
struct SubscriberConfig {
    /// Enable low-latency mode (Requirement 12.4)
    bool lowLatencyMode{false};

    /// Maximum buffer duration (default 5 seconds, 500ms for low-latency per Requirement 12.5)
    std::chrono::milliseconds maxBufferDuration{5000};

    /// Whether to preserve audio when dropping frames
    bool preserveAudio{true};

    /// Maximum buffer size in bytes (0 = unlimited)
    size_t maxBufferSize{0};

    SubscriberConfig() = default;

    /**
     * @brief Create a low-latency configuration.
     *
     * @return SubscriberConfig with low-latency settings
     */
    static SubscriberConfig lowLatency() {
        SubscriberConfig config;
        config.lowLatencyMode = true;
        config.maxBufferDuration = std::chrono::milliseconds(500);
        return config;
    }
};

// =============================================================================
// Statistics Types
// =============================================================================

/**
 * @brief Statistics for a single subscriber.
 *
 * Tracks delivery and buffer statistics for monitoring.
 */
struct SubscriberStatistics {
    SubscriberId subscriberId{0};       ///< Subscriber identifier
    StreamKey streamKey;                 ///< Associated stream key
    uint64_t pendingFrames{0};          ///< Frames waiting to be delivered
    uint64_t pendingBytes{0};           ///< Bytes waiting to be delivered
    std::chrono::milliseconds bufferLevel{0};  ///< Current buffer duration
    uint64_t bytesDelivered{0};         ///< Total bytes delivered to subscriber
    uint64_t framesDelivered{0};        ///< Total frames delivered to subscriber
    uint64_t droppedFrames{0};          ///< Total frames dropped due to overflow
    uint64_t droppedBytes{0};           ///< Total bytes dropped due to overflow
    std::chrono::steady_clock::time_point subscribedAt;  ///< Subscription time
    std::chrono::steady_clock::time_point lastDeliveryAt;  ///< Last delivery time
};

// =============================================================================
// Error Types
// =============================================================================

/**
 * @brief Subscriber manager error information.
 */
struct SubscriberManagerError {
    /**
     * @brief Error codes for subscriber manager operations.
     */
    enum class Code {
        StreamNotFound,          ///< Stream key not found in registry
        SubscriberNotFound,      ///< Subscriber ID not found
        SubscriberAlreadyExists, ///< Subscriber already exists for stream
        InvalidSubscriberId,     ///< Invalid subscriber ID provided
        BufferFull,              ///< Subscriber buffer is full
        InternalError            ///< Internal error occurred
    };

    Code code;              ///< Error code
    std::string message;    ///< Human-readable error message

    SubscriberManagerError(Code c = Code::InternalError, std::string msg = "")
        : code(c), message(std::move(msg)) {}
};

// =============================================================================
// Callback Types
// =============================================================================

/**
 * @brief Callback type for subscriber addition events.
 */
using OnSubscriberAddedCallback = std::function<void(SubscriberId, const StreamKey&)>;

/**
 * @brief Callback type for subscriber removal events.
 */
using OnSubscriberRemovedCallback = std::function<void(SubscriberId, const StreamKey&)>;

/**
 * @brief Callback type for frame dropped events.
 */
using OnSubscriberFrameDroppedCallback = std::function<void(SubscriberId, const BufferedFrame&)>;

// =============================================================================
// Subscriber Manager Interface
// =============================================================================

/**
 * @brief Interface for subscriber management operations.
 *
 * Defines the contract for managing stream subscriptions.
 */
class ISubscriberManager {
public:
    virtual ~ISubscriberManager() = default;

    // -------------------------------------------------------------------------
    // Subscriber Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Add a subscriber to a stream with default configuration.
     *
     * @param streamKey The stream to subscribe to
     * @param subscriberId The subscriber connection ID
     * @return Result indicating success or error
     */
    virtual core::Result<void, SubscriberManagerError> addSubscriber(
        const StreamKey& streamKey,
        SubscriberId subscriberId
    ) = 0;

    /**
     * @brief Add a subscriber to a stream with custom configuration.
     *
     * @param streamKey The stream to subscribe to
     * @param subscriberId The subscriber connection ID
     * @param config Subscriber configuration
     * @return Result indicating success or error
     */
    virtual core::Result<void, SubscriberManagerError> addSubscriber(
        const StreamKey& streamKey,
        SubscriberId subscriberId,
        const SubscriberConfig& config
    ) = 0;

    /**
     * @brief Remove a subscriber from its stream.
     *
     * @param subscriberId The subscriber to remove
     * @return Result indicating success or error
     */
    virtual core::Result<void, SubscriberManagerError> removeSubscriber(
        SubscriberId subscriberId
    ) = 0;

    /**
     * @brief Handle subscriber disconnect event.
     *
     * @param subscriberId The disconnected subscriber ID
     */
    virtual void onSubscriberDisconnect(SubscriberId subscriberId) = 0;

    /**
     * @brief Handle stream ended event - removes all subscribers.
     *
     * @param streamKey The ended stream
     */
    virtual void onStreamEnded(const StreamKey& streamKey) = 0;

    // -------------------------------------------------------------------------
    // Subscriber Queries
    // -------------------------------------------------------------------------

    /**
     * @brief Check if a subscriber exists.
     *
     * @param subscriberId The subscriber to check
     * @return true if subscriber exists
     */
    virtual bool hasSubscriber(SubscriberId subscriberId) const = 0;

    /**
     * @brief Get the stream key for a subscriber.
     *
     * @param subscriberId The subscriber ID
     * @return Stream key or empty if not found
     */
    virtual std::optional<StreamKey> getSubscriberStreamKey(
        SubscriberId subscriberId
    ) const = 0;

    /**
     * @brief Get all subscribers for a stream.
     *
     * @param streamKey The stream key
     * @return Set of subscriber IDs
     */
    virtual std::set<SubscriberId> getSubscribersForStream(
        const StreamKey& streamKey
    ) const = 0;

    /**
     * @brief Get subscriber count for a stream.
     *
     * @param streamKey The stream key
     * @return Number of subscribers
     */
    virtual size_t getSubscriberCount(const StreamKey& streamKey) const = 0;

    /**
     * @brief Get total subscriber count across all streams.
     *
     * @return Total number of subscribers
     */
    virtual size_t getTotalSubscriberCount() const = 0;

    // -------------------------------------------------------------------------
    // Buffer Operations
    // -------------------------------------------------------------------------

    /**
     * @brief Push a frame to a specific subscriber's buffer.
     *
     * @param subscriberId The target subscriber
     * @param frame The frame to push
     */
    virtual void pushToSubscriber(
        SubscriberId subscriberId,
        const BufferedFrame& frame
    ) = 0;

    /**
     * @brief Pop a frame from a subscriber's buffer.
     *
     * @param subscriberId The target subscriber
     * @return Frame or empty if buffer is empty
     */
    virtual std::optional<BufferedFrame> popFromSubscriber(
        SubscriberId subscriberId
    ) = 0;

    /**
     * @brief Distribute a frame to all subscribers of a stream.
     *
     * @param streamKey The target stream
     * @param frame The frame to distribute
     */
    virtual void distributeToStream(
        const StreamKey& streamKey,
        const BufferedFrame& frame
    ) = 0;

    // -------------------------------------------------------------------------
    // Statistics
    // -------------------------------------------------------------------------

    /**
     * @brief Get statistics for a specific subscriber.
     *
     * @param subscriberId The subscriber ID
     * @return Statistics or empty if not found
     */
    virtual std::optional<SubscriberStatistics> getSubscriberStats(
        SubscriberId subscriberId
    ) const = 0;

    /**
     * @brief Get statistics for all subscribers of a stream.
     *
     * @param streamKey The stream key
     * @return Vector of subscriber statistics
     */
    virtual std::vector<SubscriberStatistics> getAllSubscriberStats(
        const StreamKey& streamKey
    ) const = 0;

    /**
     * @brief Get buffer level for a subscriber.
     *
     * @param subscriberId The subscriber ID
     * @return Buffer duration or empty if not found
     */
    virtual std::optional<std::chrono::milliseconds> getSubscriberBufferLevel(
        SubscriberId subscriberId
    ) const = 0;

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    /**
     * @brief Get configuration for a subscriber.
     *
     * @param subscriberId The subscriber ID
     * @return Configuration or empty if not found
     */
    virtual std::optional<SubscriberConfig> getSubscriberConfig(
        SubscriberId subscriberId
    ) const = 0;

    /**
     * @brief Update configuration for a subscriber.
     *
     * @param subscriberId The subscriber ID
     * @param config New configuration
     * @return Result indicating success or error
     */
    virtual core::Result<void, SubscriberManagerError> setSubscriberConfig(
        SubscriberId subscriberId,
        const SubscriberConfig& config
    ) = 0;

    // -------------------------------------------------------------------------
    // Callbacks
    // -------------------------------------------------------------------------

    /**
     * @brief Set callback for subscriber added events.
     *
     * @param callback Callback function
     */
    virtual void setOnSubscriberAddedCallback(OnSubscriberAddedCallback callback) = 0;

    /**
     * @brief Set callback for subscriber removed events.
     *
     * @param callback Callback function
     */
    virtual void setOnSubscriberRemovedCallback(OnSubscriberRemovedCallback callback) = 0;

    /**
     * @brief Set callback for frame dropped events.
     *
     * @param callback Callback function
     */
    virtual void setOnFrameDroppedCallback(OnSubscriberFrameDroppedCallback callback) = 0;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Clear all subscribers.
     */
    virtual void clear() = 0;
};

// =============================================================================
// Subscriber Manager Implementation
// =============================================================================

/**
 * @brief Thread-safe subscriber manager implementation.
 *
 * Implements the ISubscriberManager interface with:
 * - Independent per-subscriber buffers (Requirement 5.4)
 * - Low-latency mode support (Requirements 12.4, 12.5)
 * - Clean removal on disconnect
 * - Statistics tracking
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses std::shared_mutex for read/write locking
 */
class SubscriberManager : public ISubscriberManager {
public:
    /**
     * @brief Construct a new SubscriberManager.
     *
     * @param streamRegistry Shared pointer to stream registry for stream lookup
     */
    explicit SubscriberManager(std::shared_ptr<IStreamRegistry> streamRegistry);

    /**
     * @brief Destructor.
     */
    ~SubscriberManager() override;

    // Non-copyable
    SubscriberManager(const SubscriberManager&) = delete;
    SubscriberManager& operator=(const SubscriberManager&) = delete;

    // Movable
    SubscriberManager(SubscriberManager&&) noexcept;
    SubscriberManager& operator=(SubscriberManager&&) noexcept;

    // ISubscriberManager interface implementation
    core::Result<void, SubscriberManagerError> addSubscriber(
        const StreamKey& streamKey,
        SubscriberId subscriberId
    ) override;

    core::Result<void, SubscriberManagerError> addSubscriber(
        const StreamKey& streamKey,
        SubscriberId subscriberId,
        const SubscriberConfig& config
    ) override;

    core::Result<void, SubscriberManagerError> removeSubscriber(
        SubscriberId subscriberId
    ) override;

    void onSubscriberDisconnect(SubscriberId subscriberId) override;
    void onStreamEnded(const StreamKey& streamKey) override;

    bool hasSubscriber(SubscriberId subscriberId) const override;

    std::optional<StreamKey> getSubscriberStreamKey(
        SubscriberId subscriberId
    ) const override;

    std::set<SubscriberId> getSubscribersForStream(
        const StreamKey& streamKey
    ) const override;

    size_t getSubscriberCount(const StreamKey& streamKey) const override;
    size_t getTotalSubscriberCount() const override;

    void pushToSubscriber(
        SubscriberId subscriberId,
        const BufferedFrame& frame
    ) override;

    std::optional<BufferedFrame> popFromSubscriber(
        SubscriberId subscriberId
    ) override;

    void distributeToStream(
        const StreamKey& streamKey,
        const BufferedFrame& frame
    ) override;

    std::optional<SubscriberStatistics> getSubscriberStats(
        SubscriberId subscriberId
    ) const override;

    std::vector<SubscriberStatistics> getAllSubscriberStats(
        const StreamKey& streamKey
    ) const override;

    std::optional<std::chrono::milliseconds> getSubscriberBufferLevel(
        SubscriberId subscriberId
    ) const override;

    std::optional<SubscriberConfig> getSubscriberConfig(
        SubscriberId subscriberId
    ) const override;

    core::Result<void, SubscriberManagerError> setSubscriberConfig(
        SubscriberId subscriberId,
        const SubscriberConfig& config
    ) override;

    void setOnSubscriberAddedCallback(OnSubscriberAddedCallback callback) override;
    void setOnSubscriberRemovedCallback(OnSubscriberRemovedCallback callback) override;
    void setOnFrameDroppedCallback(OnSubscriberFrameDroppedCallback callback) override;

    void clear() override;

private:
    /**
     * @brief Internal subscriber state.
     */
    struct SubscriberState {
        SubscriberId subscriberId;
        StreamKey streamKey;
        SubscriberConfig config;
        std::unique_ptr<SubscriberBuffer> buffer;
        uint64_t bytesDelivered{0};
        uint64_t framesDelivered{0};
        std::chrono::steady_clock::time_point subscribedAt;
        std::chrono::steady_clock::time_point lastDeliveryAt;
    };

    /**
     * @brief Convert SubscriberConfig to SubscriberBufferConfig.
     */
    SubscriberBufferConfig toBufferConfig(const SubscriberConfig& config) const;

    /**
     * @brief Emit subscriber added event.
     */
    void emitSubscriberAdded(SubscriberId subscriberId, const StreamKey& streamKey);

    /**
     * @brief Emit subscriber removed event.
     */
    void emitSubscriberRemoved(SubscriberId subscriberId, const StreamKey& streamKey);

    /**
     * @brief Setup frame drop callback for a subscriber's buffer.
     */
    void setupDropCallback(SubscriberState& state);

    // Stream registry for stream lookup
    std::shared_ptr<IStreamRegistry> streamRegistry_;

    // Subscriber storage: subscriberId -> SubscriberState
    std::unordered_map<SubscriberId, SubscriberState> subscribers_;

    // Stream to subscribers index: streamKey -> set<subscriberId>
    std::map<StreamKey, std::set<SubscriberId>> streamSubscribers_;

    // Thread safety
    mutable std::shared_mutex mutex_;

    // Callbacks
    OnSubscriberAddedCallback onSubscriberAddedCallback_;
    OnSubscriberRemovedCallback onSubscriberRemovedCallback_;
    OnSubscriberFrameDroppedCallback onFrameDroppedCallback_;
    std::mutex callbackMutex_;
};

} // namespace streaming
} // namespace openrtmp

#endif // OPENRTMP_STREAMING_SUBSCRIBER_MANAGER_HPP
