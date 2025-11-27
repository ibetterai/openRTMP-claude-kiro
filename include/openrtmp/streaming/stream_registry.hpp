// OpenRTMP - Cross-platform RTMP Server
// Stream Registry - Tracks active streams and publishers
//
// Responsibilities:
// - Maintain map of active streams by stream key
// - Track publisher and subscriber associations per stream
// - Allocate and release stream IDs atomically
// - Support concurrent access with appropriate locking
// - Emit domain events for stream lifecycle changes
//
// Requirements coverage:
// - Requirement 3.2: Allocate stream ID and return it
// - Requirement 3.7: Stream key conflict detection
// - Requirement 4.1: Store stream with associated stream key

#ifndef OPENRTMP_STREAMING_STREAM_REGISTRY_HPP
#define OPENRTMP_STREAMING_STREAM_REGISTRY_HPP

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
#include <atomic>
#include <chrono>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/result.hpp"

namespace openrtmp {
namespace streaming {

// =============================================================================
// Stream Metadata
// =============================================================================

/**
 * @brief Additional stream metadata.
 */
struct StreamMetadata {
    std::string title;
    std::string description;
    std::map<std::string, std::string> customData;

    StreamMetadata() = default;
};

// =============================================================================
// Stream Information
// =============================================================================

/**
 * @brief Complete stream information structure.
 *
 * Contains all information about a registered stream including
 * publisher, subscribers, codec info, and metadata.
 */
struct StreamInfo {
    StreamKey key;                              ///< Stream key (app/name)
    StreamId streamId;                          ///< Allocated stream ID
    StreamState state;                          ///< Current stream state
    PublisherId publisherId;                    ///< Publisher connection ID
    std::set<SubscriberId> subscribers;         ///< Set of subscriber IDs
    CodecInfo codecInfo;                        ///< Video/audio codec info
    StreamMetadata metadata;                    ///< Additional metadata
    std::chrono::steady_clock::time_point startedAt;  ///< Stream start time

    StreamInfo() = default;
    StreamInfo(const StreamKey& k, StreamId id, PublisherId pub)
        : key(k)
        , streamId(id)
        , state(StreamState::Publishing)
        , publisherId(pub)
        , startedAt(std::chrono::steady_clock::now())
    {}
};

// =============================================================================
// Domain Events
// =============================================================================

/**
 * @brief Stream lifecycle event.
 *
 * Emitted when stream state changes occur.
 */
struct StreamEvent {
    /**
     * @brief Event types for stream lifecycle changes.
     */
    enum class Type {
        StreamPublished,    ///< A new stream started publishing
        StreamEnded,        ///< A stream stopped publishing
        SubscriberJoined,   ///< A subscriber joined a stream
        SubscriberLeft      ///< A subscriber left a stream
    };

    Type type;                  ///< Event type
    StreamKey streamKey;        ///< Stream key involved
    StreamId streamId;          ///< Stream ID involved
    PublisherId publisherId;    ///< Publisher ID (for publish events)
    SubscriberId subscriberId;  ///< Subscriber ID (for subscriber events)
    std::chrono::steady_clock::time_point timestamp;  ///< Event timestamp

    StreamEvent()
        : type(Type::StreamPublished)
        , streamId(0)
        , publisherId(0)
        , subscriberId(0)
        , timestamp(std::chrono::steady_clock::now())
    {}

    static StreamEvent streamPublished(const StreamKey& key, StreamId id, PublisherId pub) {
        StreamEvent event;
        event.type = Type::StreamPublished;
        event.streamKey = key;
        event.streamId = id;
        event.publisherId = pub;
        return event;
    }

    static StreamEvent streamEnded(const StreamKey& key, StreamId id, PublisherId pub) {
        StreamEvent event;
        event.type = Type::StreamEnded;
        event.streamKey = key;
        event.streamId = id;
        event.publisherId = pub;
        return event;
    }

    static StreamEvent subscriberJoined(const StreamKey& key, StreamId id, SubscriberId sub) {
        StreamEvent event;
        event.type = Type::SubscriberJoined;
        event.streamKey = key;
        event.streamId = id;
        event.subscriberId = sub;
        return event;
    }

    static StreamEvent subscriberLeft(const StreamKey& key, StreamId id, SubscriberId sub) {
        StreamEvent event;
        event.type = Type::SubscriberLeft;
        event.streamKey = key;
        event.streamId = id;
        event.subscriberId = sub;
        return event;
    }
};

/**
 * @brief Callback type for stream events.
 */
using StreamEventCallback = std::function<void(const StreamEvent&)>;

// =============================================================================
// Error Types
// =============================================================================

/**
 * @brief Stream registry error information.
 */
struct StreamRegistryError {
    /**
     * @brief Error codes for stream registry operations.
     */
    enum class Code {
        StreamKeyInUse,      ///< Stream key already registered (Requirement 3.7)
        StreamNotFound,      ///< Stream key not found
        InvalidStreamId,     ///< Invalid stream ID provided
        SubscriberExists,    ///< Subscriber already exists for stream
        SubscriberNotFound,  ///< Subscriber not found for stream
        InternalError        ///< Internal error occurred
    };

    Code code;              ///< Error code
    std::string message;    ///< Human-readable error message

    StreamRegistryError(Code c = Code::InternalError, std::string msg = "")
        : code(c), message(std::move(msg)) {}
};

// =============================================================================
// Stream Registry Interface
// =============================================================================

/**
 * @brief Interface for stream registry operations.
 *
 * Defines the contract for managing active streams.
 */
class IStreamRegistry {
public:
    virtual ~IStreamRegistry() = default;

    // -------------------------------------------------------------------------
    // Stream ID Management
    // -------------------------------------------------------------------------

    /**
     * @brief Allocate a new unique stream ID.
     *
     * Stream IDs are allocated atomically and are guaranteed to be unique
     * within the lifetime of the registry.
     *
     * @return Result containing the allocated stream ID or an error
     */
    virtual core::Result<StreamId, StreamRegistryError> allocateStreamId() = 0;

    /**
     * @brief Release a previously allocated stream ID.
     *
     * @param streamId The stream ID to release
     */
    virtual void releaseStreamId(StreamId streamId) = 0;

    // -------------------------------------------------------------------------
    // Stream Registration
    // -------------------------------------------------------------------------

    /**
     * @brief Register a new stream with the given key.
     *
     * Creates a new stream entry and associates it with the publisher.
     * Fails if the stream key is already in use (Requirement 3.7).
     *
     * @param key The stream key (app/name)
     * @param streamId The allocated stream ID
     * @param publisherId The publisher connection ID
     * @return Result indicating success or error (StreamKeyInUse)
     */
    virtual core::Result<void, StreamRegistryError> registerStream(
        const StreamKey& key,
        StreamId streamId,
        PublisherId publisherId
    ) = 0;

    /**
     * @brief Unregister a stream by key.
     *
     * Removes the stream entry and notifies all subscribers.
     *
     * @param key The stream key to unregister
     * @return Result indicating success or error (StreamNotFound)
     */
    virtual core::Result<void, StreamRegistryError> unregisterStream(
        const StreamKey& key
    ) = 0;

    // -------------------------------------------------------------------------
    // Stream Lookup
    // -------------------------------------------------------------------------

    /**
     * @brief Find a stream by its key.
     *
     * @param key The stream key to find
     * @return Optional containing stream info if found
     */
    virtual std::optional<StreamInfo> findStream(const StreamKey& key) const = 0;

    /**
     * @brief Find a stream by its ID.
     *
     * @param streamId The stream ID to find
     * @return Optional containing stream info if found
     */
    virtual std::optional<StreamInfo> findStreamById(StreamId streamId) const = 0;

    /**
     * @brief Check if a stream exists.
     *
     * @param key The stream key to check
     * @return true if stream exists
     */
    virtual bool hasStream(const StreamKey& key) const = 0;

    // -------------------------------------------------------------------------
    // Subscriber Management
    // -------------------------------------------------------------------------

    /**
     * @brief Add a subscriber to a stream.
     *
     * @param key The stream key
     * @param subscriberId The subscriber connection ID
     * @return Result indicating success or error
     */
    virtual core::Result<void, StreamRegistryError> addSubscriber(
        const StreamKey& key,
        SubscriberId subscriberId
    ) = 0;

    /**
     * @brief Remove a subscriber from a stream.
     *
     * @param key The stream key
     * @param subscriberId The subscriber connection ID
     * @return Result indicating success or error
     */
    virtual core::Result<void, StreamRegistryError> removeSubscriber(
        const StreamKey& key,
        SubscriberId subscriberId
    ) = 0;

    /**
     * @brief Get all subscribers for a stream.
     *
     * @param key The stream key
     * @return Set of subscriber IDs
     */
    virtual std::set<SubscriberId> getSubscribers(const StreamKey& key) const = 0;

    /**
     * @brief Get subscriber count for a stream.
     *
     * @param key The stream key
     * @return Number of subscribers
     */
    virtual size_t getSubscriberCount(const StreamKey& key) const = 0;

    // -------------------------------------------------------------------------
    // Metadata Updates
    // -------------------------------------------------------------------------

    /**
     * @brief Update stream metadata.
     *
     * @param key The stream key
     * @param metadata The new metadata
     * @return Result indicating success or error
     */
    virtual core::Result<void, StreamRegistryError> updateMetadata(
        const StreamKey& key,
        const StreamMetadata& metadata
    ) = 0;

    /**
     * @brief Update stream codec information.
     *
     * @param key The stream key
     * @param codecInfo The new codec info
     * @return Result indicating success or error
     */
    virtual core::Result<void, StreamRegistryError> updateCodecInfo(
        const StreamKey& key,
        const CodecInfo& codecInfo
    ) = 0;

    // -------------------------------------------------------------------------
    // Statistics
    // -------------------------------------------------------------------------

    /**
     * @brief Get the number of active streams.
     *
     * @return Count of active streams
     */
    virtual size_t getActiveStreamCount() const = 0;

    /**
     * @brief Get all active stream keys.
     *
     * @return Vector of active stream keys
     */
    virtual std::vector<StreamKey> getAllStreamKeys() const = 0;

    // -------------------------------------------------------------------------
    // Event Handling
    // -------------------------------------------------------------------------

    /**
     * @brief Set the event callback.
     *
     * @param callback Callback to invoke on stream events
     */
    virtual void setEventCallback(StreamEventCallback callback) = 0;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Clear all streams and reset state.
     */
    virtual void clear() = 0;
};

// =============================================================================
// Stream Registry Implementation
// =============================================================================

/**
 * @brief Thread-safe stream registry implementation.
 *
 * Implements the IStreamRegistry interface with:
 * - Atomic stream ID allocation
 * - Thread-safe concurrent access using shared_mutex
 * - Event emission for stream lifecycle changes
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses std::shared_mutex for read/write locking
 * - Stream ID allocation uses std::atomic
 */
class StreamRegistry : public IStreamRegistry {
public:
    /**
     * @brief Construct a new StreamRegistry.
     */
    StreamRegistry();

    /**
     * @brief Destructor.
     */
    ~StreamRegistry() override;

    // Non-copyable
    StreamRegistry(const StreamRegistry&) = delete;
    StreamRegistry& operator=(const StreamRegistry&) = delete;

    // Movable
    StreamRegistry(StreamRegistry&&) noexcept;
    StreamRegistry& operator=(StreamRegistry&&) noexcept;

    // IStreamRegistry interface implementation
    core::Result<StreamId, StreamRegistryError> allocateStreamId() override;
    void releaseStreamId(StreamId streamId) override;

    core::Result<void, StreamRegistryError> registerStream(
        const StreamKey& key,
        StreamId streamId,
        PublisherId publisherId
    ) override;

    core::Result<void, StreamRegistryError> unregisterStream(
        const StreamKey& key
    ) override;

    std::optional<StreamInfo> findStream(const StreamKey& key) const override;
    std::optional<StreamInfo> findStreamById(StreamId streamId) const override;
    bool hasStream(const StreamKey& key) const override;

    core::Result<void, StreamRegistryError> addSubscriber(
        const StreamKey& key,
        SubscriberId subscriberId
    ) override;

    core::Result<void, StreamRegistryError> removeSubscriber(
        const StreamKey& key,
        SubscriberId subscriberId
    ) override;

    std::set<SubscriberId> getSubscribers(const StreamKey& key) const override;
    size_t getSubscriberCount(const StreamKey& key) const override;

    core::Result<void, StreamRegistryError> updateMetadata(
        const StreamKey& key,
        const StreamMetadata& metadata
    ) override;

    core::Result<void, StreamRegistryError> updateCodecInfo(
        const StreamKey& key,
        const CodecInfo& codecInfo
    ) override;

    size_t getActiveStreamCount() const override;
    std::vector<StreamKey> getAllStreamKeys() const override;

    void setEventCallback(StreamEventCallback callback) override;

    void clear() override;

private:
    /**
     * @brief Emit a stream event to the registered callback.
     *
     * @param event The event to emit
     */
    void emitEvent(const StreamEvent& event);

    // Stream storage
    std::unordered_map<StreamKey, StreamInfo> streams_;

    // Index for stream ID -> stream key lookup
    std::unordered_map<StreamId, StreamKey> streamIdIndex_;

    // Atomic counter for stream ID allocation
    std::atomic<StreamId> nextStreamId_;

    // Released stream IDs that can be reused (optional optimization)
    std::vector<StreamId> releasedIds_;
    std::mutex releasedIdsMutex_;

    // Read/write lock for streams map
    mutable std::shared_mutex streamsMutex_;

    // Event callback
    StreamEventCallback eventCallback_;
    std::mutex callbackMutex_;
};

} // namespace streaming
} // namespace openrtmp

#endif // OPENRTMP_STREAMING_STREAM_REGISTRY_HPP
