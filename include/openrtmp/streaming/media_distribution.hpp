// OpenRTMP - Cross-platform RTMP Server
// Media Distribution Engine - Delivers media to all subscribers
//
// Responsibilities:
// - Send cached metadata and sequence headers on subscription start
// - Transmit from most recent keyframe for instant playback
// - Forward live media from ingestion pipeline to all subscribers
// - Send stream EOF message within 1 second of stream end
// - Implement slow subscriber detection and frame dropping
//
// Requirements coverage:
// - Requirement 5.1: Transmit data starting from most recent keyframe
// - Requirement 5.3: Send cached metadata and sequence headers before stream data
// - Requirement 5.6: Send stream EOF message within 1 second of stream end

#ifndef OPENRTMP_STREAMING_MEDIA_DISTRIBUTION_HPP
#define OPENRTMP_STREAMING_MEDIA_DISTRIBUTION_HPP

#include <cstdint>
#include <string>
#include <optional>
#include <memory>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <chrono>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/result.hpp"
#include "openrtmp/streaming/gop_buffer.hpp"
#include "openrtmp/streaming/subscriber_manager.hpp"
#include "openrtmp/streaming/stream_registry.hpp"
#include "openrtmp/protocol/amf_codec.hpp"

namespace openrtmp {
namespace streaming {

// =============================================================================
// Error Types
// =============================================================================

/**
 * @brief Media distribution error information.
 */
struct MediaDistributionError {
    /**
     * @brief Error codes for media distribution operations.
     */
    enum class Code {
        StreamNotFound,          ///< Stream key not found
        SubscriberNotFound,      ///< Subscriber ID not found
        SubscriberAlreadyExists, ///< Subscriber already subscribed
        GOPBufferNotFound,       ///< GOP buffer not set for stream
        InternalError            ///< Internal error occurred
    };

    Code code;              ///< Error code
    std::string message;    ///< Human-readable error message

    MediaDistributionError(Code c = Code::InternalError, std::string msg = "")
        : code(c), message(std::move(msg)) {}
};

// =============================================================================
// Statistics Types
// =============================================================================

/**
 * @brief Distribution statistics for a stream.
 */
struct DistributionStats {
    StreamKey streamKey;                ///< Stream key
    size_t subscriberCount{0};          ///< Current subscriber count
    uint64_t totalFramesDistributed{0}; ///< Total frames distributed
    uint64_t totalBytesDistributed{0};  ///< Total bytes distributed
    uint64_t droppedFrames{0};          ///< Total frames dropped (slow subscribers)
    std::chrono::steady_clock::time_point startedAt;  ///< Distribution start time
};

// =============================================================================
// Callback Types
// =============================================================================

/**
 * @brief Callback invoked when a frame is sent to a subscriber.
 */
using OnFrameSentCallback = std::function<void(SubscriberId, const BufferedFrame&)>;

/**
 * @brief Callback invoked when stream EOF is sent to a subscriber.
 */
using OnStreamEOFCallback = std::function<void(SubscriberId, const StreamKey&)>;

/**
 * @brief Callback invoked when a slow subscriber is detected.
 */
using OnSlowSubscriberCallback = std::function<void(SubscriberId)>;

// =============================================================================
// Media Distribution Interface
// =============================================================================

/**
 * @brief Interface for media distribution operations.
 *
 * Defines the contract for distributing media to subscribers.
 */
class IMediaDistribution {
public:
    virtual ~IMediaDistribution() = default;

    // -------------------------------------------------------------------------
    // GOP Buffer Management
    // -------------------------------------------------------------------------

    /**
     * @brief Set the GOP buffer for a stream.
     *
     * @param streamKey The stream key
     * @param buffer Shared pointer to GOP buffer
     */
    virtual void setGOPBuffer(
        const StreamKey& streamKey,
        std::shared_ptr<IGOPBuffer> buffer
    ) = 0;

    /**
     * @brief Get the GOP buffer for a stream.
     *
     * @param streamKey The stream key
     * @return Shared pointer to GOP buffer or nullptr
     */
    virtual std::shared_ptr<IGOPBuffer> getGOPBuffer(
        const StreamKey& streamKey
    ) const = 0;

    // -------------------------------------------------------------------------
    // Subscriber Management
    // -------------------------------------------------------------------------

    /**
     * @brief Add a subscriber to a stream with default configuration.
     *
     * Sends cached metadata, sequence headers, and frames from last keyframe.
     *
     * @param streamKey The stream to subscribe to
     * @param subscriberId The subscriber connection ID
     * @return Result indicating success or error
     */
    virtual core::Result<void, MediaDistributionError> addSubscriber(
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
    virtual core::Result<void, MediaDistributionError> addSubscriber(
        const StreamKey& streamKey,
        SubscriberId subscriberId,
        const SubscriberConfig& config
    ) = 0;

    /**
     * @brief Remove a subscriber.
     *
     * @param subscriberId The subscriber to remove
     * @return Result indicating success or error
     */
    virtual core::Result<void, MediaDistributionError> removeSubscriber(
        SubscriberId subscriberId
    ) = 0;

    /**
     * @brief Check if a subscriber exists.
     *
     * @param subscriberId The subscriber ID
     * @return true if subscriber exists
     */
    virtual bool hasSubscriber(SubscriberId subscriberId) const = 0;

    /**
     * @brief Get subscriber count for a stream.
     *
     * @param streamKey The stream key
     * @return Number of subscribers
     */
    virtual size_t getSubscriberCount(const StreamKey& streamKey) const = 0;

    // -------------------------------------------------------------------------
    // Media Distribution
    // -------------------------------------------------------------------------

    /**
     * @brief Distribute a frame to all subscribers of a stream.
     *
     * Also updates the GOP buffer with the new frame.
     *
     * @param streamKey The stream key
     * @param frame The frame to distribute
     */
    virtual void distribute(
        const StreamKey& streamKey,
        const BufferedFrame& frame
    ) = 0;

    /**
     * @brief Set metadata for a stream (cached for new subscribers).
     *
     * @param streamKey The stream key
     * @param metadata The AMF metadata
     */
    virtual void setMetadata(
        const StreamKey& streamKey,
        const protocol::AMFValue& metadata
    ) = 0;

    /**
     * @brief Set sequence headers for a stream (cached for new subscribers).
     *
     * @param streamKey The stream key
     * @param videoHeader Video sequence header
     * @param audioHeader Audio sequence header
     */
    virtual void setSequenceHeaders(
        const StreamKey& streamKey,
        const std::vector<uint8_t>& videoHeader,
        const std::vector<uint8_t>& audioHeader
    ) = 0;

    // -------------------------------------------------------------------------
    // Stream Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Handle stream end - send EOF to all subscribers.
     *
     * Must send EOF within 1 second per Requirement 5.6.
     *
     * @param streamKey The stream that ended
     */
    virtual void onStreamEnd(const StreamKey& streamKey) = 0;

    // -------------------------------------------------------------------------
    // Statistics
    // -------------------------------------------------------------------------

    /**
     * @brief Get distribution statistics for a stream.
     *
     * @param streamKey The stream key
     * @return Statistics or empty if stream not found
     */
    virtual std::optional<DistributionStats> getDistributionStats(
        const StreamKey& streamKey
    ) const = 0;

    // -------------------------------------------------------------------------
    // Callbacks
    // -------------------------------------------------------------------------

    /**
     * @brief Set callback for frame sent events.
     *
     * @param callback Callback function
     */
    virtual void setOnFrameSentCallback(OnFrameSentCallback callback) = 0;

    /**
     * @brief Set callback for stream EOF events.
     *
     * @param callback Callback function
     */
    virtual void setOnStreamEOFCallback(OnStreamEOFCallback callback) = 0;

    /**
     * @brief Set callback for slow subscriber detection.
     *
     * @param callback Callback function
     */
    virtual void setOnSlowSubscriberCallback(OnSlowSubscriberCallback callback) = 0;
};

// =============================================================================
// Media Distribution Implementation
// =============================================================================

/**
 * @brief Thread-safe media distribution implementation.
 *
 * Implements IMediaDistribution with:
 * - Metadata and sequence header caching via GOP buffer
 * - Instant playback from most recent keyframe
 * - Live media forwarding to all subscribers
 * - EOF notification within 1 second of stream end
 * - Slow subscriber detection and frame dropping
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses SubscriberManager for subscriber buffer management
 */
class MediaDistribution : public IMediaDistribution {
public:
    /**
     * @brief Construct a new MediaDistribution.
     *
     * @param streamRegistry Shared pointer to stream registry
     * @param subscriberManager Shared pointer to subscriber manager
     */
    MediaDistribution(
        std::shared_ptr<IStreamRegistry> streamRegistry,
        std::shared_ptr<ISubscriberManager> subscriberManager
    );

    /**
     * @brief Destructor.
     */
    ~MediaDistribution() override;

    // Non-copyable
    MediaDistribution(const MediaDistribution&) = delete;
    MediaDistribution& operator=(const MediaDistribution&) = delete;

    // Movable
    MediaDistribution(MediaDistribution&&) noexcept;
    MediaDistribution& operator=(MediaDistribution&&) noexcept;

    // IMediaDistribution interface implementation
    void setGOPBuffer(
        const StreamKey& streamKey,
        std::shared_ptr<IGOPBuffer> buffer
    ) override;

    std::shared_ptr<IGOPBuffer> getGOPBuffer(
        const StreamKey& streamKey
    ) const override;

    core::Result<void, MediaDistributionError> addSubscriber(
        const StreamKey& streamKey,
        SubscriberId subscriberId
    ) override;

    core::Result<void, MediaDistributionError> addSubscriber(
        const StreamKey& streamKey,
        SubscriberId subscriberId,
        const SubscriberConfig& config
    ) override;

    core::Result<void, MediaDistributionError> removeSubscriber(
        SubscriberId subscriberId
    ) override;

    bool hasSubscriber(SubscriberId subscriberId) const override;

    size_t getSubscriberCount(const StreamKey& streamKey) const override;

    void distribute(
        const StreamKey& streamKey,
        const BufferedFrame& frame
    ) override;

    void setMetadata(
        const StreamKey& streamKey,
        const protocol::AMFValue& metadata
    ) override;

    void setSequenceHeaders(
        const StreamKey& streamKey,
        const std::vector<uint8_t>& videoHeader,
        const std::vector<uint8_t>& audioHeader
    ) override;

    void onStreamEnd(const StreamKey& streamKey) override;

    std::optional<DistributionStats> getDistributionStats(
        const StreamKey& streamKey
    ) const override;

    void setOnFrameSentCallback(OnFrameSentCallback callback) override;
    void setOnStreamEOFCallback(OnStreamEOFCallback callback) override;
    void setOnSlowSubscriberCallback(OnSlowSubscriberCallback callback) override;

private:
    /**
     * @brief Internal state for a stream's distribution.
     */
    struct StreamDistributionState {
        StreamKey streamKey;
        std::shared_ptr<IGOPBuffer> gopBuffer;
        uint64_t totalFramesDistributed{0};
        uint64_t totalBytesDistributed{0};
        uint64_t droppedFrames{0};
        std::chrono::steady_clock::time_point startedAt;
        bool metadataSent{false};
        bool headersSent{false};
    };

    /**
     * @brief Send cached data to a new subscriber.
     *
     * Sends metadata, sequence headers, and frames from last keyframe.
     *
     * @param subscriberId The subscriber ID
     * @param gopBuffer The stream's GOP buffer
     */
    void sendCachedDataToSubscriber(
        SubscriberId subscriberId,
        const std::shared_ptr<IGOPBuffer>& gopBuffer
    );

    /**
     * @brief Send a frame to a subscriber.
     *
     * @param subscriberId The subscriber ID
     * @param frame The frame to send
     */
    void sendFrameToSubscriber(
        SubscriberId subscriberId,
        const BufferedFrame& frame
    );

    /**
     * @brief Check and handle slow subscriber.
     *
     * @param subscriberId The subscriber ID
     * @return true if subscriber is slow
     */
    bool checkSlowSubscriber(SubscriberId subscriberId);

    /**
     * @brief Emit frame sent callback.
     */
    void emitFrameSent(SubscriberId subscriberId, const BufferedFrame& frame);

    /**
     * @brief Emit stream EOF callback.
     */
    void emitStreamEOF(SubscriberId subscriberId, const StreamKey& streamKey);

    /**
     * @brief Emit slow subscriber callback.
     */
    void emitSlowSubscriber(SubscriberId subscriberId);

    // Dependencies
    std::shared_ptr<IStreamRegistry> streamRegistry_;
    std::shared_ptr<ISubscriberManager> subscriberManager_;

    // Stream distribution state
    std::map<StreamKey, StreamDistributionState> streamStates_;
    mutable std::shared_mutex streamStatesMutex_;

    // Callbacks
    OnFrameSentCallback onFrameSentCallback_;
    OnStreamEOFCallback onStreamEOFCallback_;
    OnSlowSubscriberCallback onSlowSubscriberCallback_;
    mutable std::mutex callbackMutex_;

    // Configuration
    std::chrono::milliseconds slowSubscriberThreshold_{5000};
};

} // namespace streaming
} // namespace openrtmp

#endif // OPENRTMP_STREAMING_MEDIA_DISTRIBUTION_HPP
