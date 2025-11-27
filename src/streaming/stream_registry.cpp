// OpenRTMP - Cross-platform RTMP Server
// Stream Registry Implementation
//
// Thread-safe implementation of stream registry with:
// - Atomic stream ID allocation
// - Concurrent access using shared_mutex (read/write locking)
// - Domain event emission for stream lifecycle changes
//
// Requirements coverage:
// - Requirement 3.2: Allocate stream ID and return it
// - Requirement 3.7: Stream key conflict detection
// - Requirement 4.1: Store stream with associated stream key

#include "openrtmp/streaming/stream_registry.hpp"

#include <algorithm>

namespace openrtmp {
namespace streaming {

// =============================================================================
// Constructor / Destructor
// =============================================================================

StreamRegistry::StreamRegistry()
    : nextStreamId_(1)  // Stream IDs start from 1 (0 is invalid)
{
}

StreamRegistry::~StreamRegistry() = default;

StreamRegistry::StreamRegistry(StreamRegistry&& other) noexcept
    : streams_(std::move(other.streams_))
    , streamIdIndex_(std::move(other.streamIdIndex_))
    , nextStreamId_(other.nextStreamId_.load())
    , releasedIds_(std::move(other.releasedIds_))
{
    std::lock_guard<std::mutex> lock(other.callbackMutex_);
    eventCallback_ = std::move(other.eventCallback_);
}

StreamRegistry& StreamRegistry::operator=(StreamRegistry&& other) noexcept {
    if (this != &other) {
        // Lock both registries
        std::unique_lock<std::shared_mutex> lock1(streamsMutex_, std::defer_lock);
        std::unique_lock<std::shared_mutex> lock2(other.streamsMutex_, std::defer_lock);
        std::lock(lock1, lock2);

        streams_ = std::move(other.streams_);
        streamIdIndex_ = std::move(other.streamIdIndex_);
        nextStreamId_ = other.nextStreamId_.load();

        {
            std::lock_guard<std::mutex> releasedLock(releasedIdsMutex_);
            std::lock_guard<std::mutex> otherReleasedLock(other.releasedIdsMutex_);
            releasedIds_ = std::move(other.releasedIds_);
        }

        {
            std::lock_guard<std::mutex> cbLock(callbackMutex_);
            std::lock_guard<std::mutex> otherCbLock(other.callbackMutex_);
            eventCallback_ = std::move(other.eventCallback_);
        }
    }
    return *this;
}

// =============================================================================
// Stream ID Management
// =============================================================================

core::Result<StreamId, StreamRegistryError> StreamRegistry::allocateStreamId() {
    // First, check if we have any released IDs to reuse
    {
        std::lock_guard<std::mutex> lock(releasedIdsMutex_);
        if (!releasedIds_.empty()) {
            StreamId id = releasedIds_.back();
            releasedIds_.pop_back();
            return core::Result<StreamId, StreamRegistryError>::success(id);
        }
    }

    // Allocate new ID atomically
    StreamId id = nextStreamId_.fetch_add(1, std::memory_order_relaxed);

    // Check for overflow (wrap-around to 0 would be invalid)
    if (id == 0) {
        // This is extremely unlikely but handle it gracefully
        id = nextStreamId_.fetch_add(1, std::memory_order_relaxed);
    }

    return core::Result<StreamId, StreamRegistryError>::success(id);
}

void StreamRegistry::releaseStreamId(StreamId streamId) {
    if (streamId == core::INVALID_STREAM_ID) {
        return;
    }

    std::lock_guard<std::mutex> lock(releasedIdsMutex_);
    releasedIds_.push_back(streamId);
}

// =============================================================================
// Stream Registration
// =============================================================================

core::Result<void, StreamRegistryError> StreamRegistry::registerStream(
    const StreamKey& key,
    StreamId streamId,
    PublisherId publisherId
) {
    std::unique_lock<std::shared_mutex> lock(streamsMutex_);

    // Check if stream key already exists (Requirement 3.7)
    if (streams_.count(key) > 0) {
        return core::Result<void, StreamRegistryError>::error(
            StreamRegistryError{
                StreamRegistryError::Code::StreamKeyInUse,
                "Stream key '" + key.toString() + "' is already in use"
            }
        );
    }

    // Create and insert stream info
    StreamInfo info(key, streamId, publisherId);
    streams_.emplace(key, info);
    streamIdIndex_.emplace(streamId, key);

    // Unlock before emitting event to avoid holding lock during callback
    lock.unlock();

    // Emit stream published event
    emitEvent(StreamEvent::streamPublished(key, streamId, publisherId));

    return core::Result<void, StreamRegistryError>::success();
}

core::Result<void, StreamRegistryError> StreamRegistry::unregisterStream(
    const StreamKey& key
) {
    StreamInfo streamInfo;

    {
        std::unique_lock<std::shared_mutex> lock(streamsMutex_);

        auto it = streams_.find(key);
        if (it == streams_.end()) {
            return core::Result<void, StreamRegistryError>::error(
                StreamRegistryError{
                    StreamRegistryError::Code::StreamNotFound,
                    "Stream key '" + key.toString() + "' not found"
                }
            );
        }

        // Copy stream info before removing
        streamInfo = it->second;

        // Remove from indexes
        streamIdIndex_.erase(streamInfo.streamId);
        streams_.erase(it);
    }

    // Emit stream ended event
    emitEvent(StreamEvent::streamEnded(key, streamInfo.streamId, streamInfo.publisherId));

    return core::Result<void, StreamRegistryError>::success();
}

// =============================================================================
// Stream Lookup
// =============================================================================

std::optional<StreamInfo> StreamRegistry::findStream(const StreamKey& key) const {
    std::shared_lock<std::shared_mutex> lock(streamsMutex_);

    auto it = streams_.find(key);
    if (it == streams_.end()) {
        return std::nullopt;
    }

    return it->second;
}

std::optional<StreamInfo> StreamRegistry::findStreamById(StreamId streamId) const {
    std::shared_lock<std::shared_mutex> lock(streamsMutex_);

    auto indexIt = streamIdIndex_.find(streamId);
    if (indexIt == streamIdIndex_.end()) {
        return std::nullopt;
    }

    auto streamIt = streams_.find(indexIt->second);
    if (streamIt == streams_.end()) {
        return std::nullopt;
    }

    return streamIt->second;
}

bool StreamRegistry::hasStream(const StreamKey& key) const {
    std::shared_lock<std::shared_mutex> lock(streamsMutex_);
    return streams_.count(key) > 0;
}

// =============================================================================
// Subscriber Management
// =============================================================================

core::Result<void, StreamRegistryError> StreamRegistry::addSubscriber(
    const StreamKey& key,
    SubscriberId subscriberId
) {
    StreamId streamId;

    {
        std::unique_lock<std::shared_mutex> lock(streamsMutex_);

        auto it = streams_.find(key);
        if (it == streams_.end()) {
            return core::Result<void, StreamRegistryError>::error(
                StreamRegistryError{
                    StreamRegistryError::Code::StreamNotFound,
                    "Stream key '" + key.toString() + "' not found"
                }
            );
        }

        // Check if subscriber already exists
        if (it->second.subscribers.count(subscriberId) > 0) {
            return core::Result<void, StreamRegistryError>::error(
                StreamRegistryError{
                    StreamRegistryError::Code::SubscriberExists,
                    "Subscriber " + std::to_string(subscriberId) + " already exists"
                }
            );
        }

        // Add subscriber
        it->second.subscribers.insert(subscriberId);
        streamId = it->second.streamId;
    }

    // Emit subscriber joined event
    emitEvent(StreamEvent::subscriberJoined(key, streamId, subscriberId));

    return core::Result<void, StreamRegistryError>::success();
}

core::Result<void, StreamRegistryError> StreamRegistry::removeSubscriber(
    const StreamKey& key,
    SubscriberId subscriberId
) {
    StreamId streamId;

    {
        std::unique_lock<std::shared_mutex> lock(streamsMutex_);

        auto it = streams_.find(key);
        if (it == streams_.end()) {
            return core::Result<void, StreamRegistryError>::error(
                StreamRegistryError{
                    StreamRegistryError::Code::StreamNotFound,
                    "Stream key '" + key.toString() + "' not found"
                }
            );
        }

        // Check if subscriber exists
        auto subIt = it->second.subscribers.find(subscriberId);
        if (subIt == it->second.subscribers.end()) {
            return core::Result<void, StreamRegistryError>::error(
                StreamRegistryError{
                    StreamRegistryError::Code::SubscriberNotFound,
                    "Subscriber " + std::to_string(subscriberId) + " not found"
                }
            );
        }

        // Remove subscriber
        it->second.subscribers.erase(subIt);
        streamId = it->second.streamId;
    }

    // Emit subscriber left event
    emitEvent(StreamEvent::subscriberLeft(key, streamId, subscriberId));

    return core::Result<void, StreamRegistryError>::success();
}

std::set<SubscriberId> StreamRegistry::getSubscribers(const StreamKey& key) const {
    std::shared_lock<std::shared_mutex> lock(streamsMutex_);

    auto it = streams_.find(key);
    if (it == streams_.end()) {
        return {};
    }

    return it->second.subscribers;
}

size_t StreamRegistry::getSubscriberCount(const StreamKey& key) const {
    std::shared_lock<std::shared_mutex> lock(streamsMutex_);

    auto it = streams_.find(key);
    if (it == streams_.end()) {
        return 0;
    }

    return it->second.subscribers.size();
}

// =============================================================================
// Metadata Updates
// =============================================================================

core::Result<void, StreamRegistryError> StreamRegistry::updateMetadata(
    const StreamKey& key,
    const StreamMetadata& metadata
) {
    std::unique_lock<std::shared_mutex> lock(streamsMutex_);

    auto it = streams_.find(key);
    if (it == streams_.end()) {
        return core::Result<void, StreamRegistryError>::error(
            StreamRegistryError{
                StreamRegistryError::Code::StreamNotFound,
                "Stream key '" + key.toString() + "' not found"
            }
        );
    }

    it->second.metadata = metadata;

    return core::Result<void, StreamRegistryError>::success();
}

core::Result<void, StreamRegistryError> StreamRegistry::updateCodecInfo(
    const StreamKey& key,
    const CodecInfo& codecInfo
) {
    std::unique_lock<std::shared_mutex> lock(streamsMutex_);

    auto it = streams_.find(key);
    if (it == streams_.end()) {
        return core::Result<void, StreamRegistryError>::error(
            StreamRegistryError{
                StreamRegistryError::Code::StreamNotFound,
                "Stream key '" + key.toString() + "' not found"
            }
        );
    }

    it->second.codecInfo = codecInfo;

    return core::Result<void, StreamRegistryError>::success();
}

// =============================================================================
// Statistics
// =============================================================================

size_t StreamRegistry::getActiveStreamCount() const {
    std::shared_lock<std::shared_mutex> lock(streamsMutex_);
    return streams_.size();
}

std::vector<StreamKey> StreamRegistry::getAllStreamKeys() const {
    std::shared_lock<std::shared_mutex> lock(streamsMutex_);

    std::vector<StreamKey> keys;
    keys.reserve(streams_.size());

    for (const auto& pair : streams_) {
        keys.push_back(pair.first);
    }

    return keys;
}

// =============================================================================
// Event Handling
// =============================================================================

void StreamRegistry::setEventCallback(StreamEventCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    eventCallback_ = std::move(callback);
}

void StreamRegistry::emitEvent(const StreamEvent& event) {
    StreamEventCallback callback;

    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = eventCallback_;
    }

    if (callback) {
        callback(event);
    }
}

// =============================================================================
// Lifecycle
// =============================================================================

void StreamRegistry::clear() {
    std::unique_lock<std::shared_mutex> lock(streamsMutex_);

    streams_.clear();
    streamIdIndex_.clear();

    {
        std::lock_guard<std::mutex> releasedLock(releasedIdsMutex_);
        releasedIds_.clear();
    }

    // Reset stream ID counter
    nextStreamId_ = 1;
}

} // namespace streaming
} // namespace openrtmp
