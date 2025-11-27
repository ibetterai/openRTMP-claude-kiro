// OpenRTMP - Cross-platform RTMP Server
// Subscriber Manager Implementation
//
// Thread-safe implementation of subscriber management with:
// - Independent per-subscriber buffers (Requirement 5.4)
// - Low-latency mode support (Requirements 12.4, 12.5)
// - Clean removal on disconnect
// - Statistics tracking
//
// Requirements coverage:
// - Requirement 5.2: Support multiple simultaneous subscribers for a single stream
// - Requirement 5.4: Maintain independent send buffers for each subscriber
// - Requirement 12.4: Low-latency mode option reducing buffering
// - Requirement 12.5: Low-latency mode 500ms maximum buffer per subscriber

#include "openrtmp/streaming/subscriber_manager.hpp"
#include "openrtmp/streaming/stream_registry.hpp"

namespace openrtmp {
namespace streaming {

// =============================================================================
// Constructor / Destructor
// =============================================================================

SubscriberManager::SubscriberManager(std::shared_ptr<IStreamRegistry> streamRegistry)
    : streamRegistry_(std::move(streamRegistry))
{
}

SubscriberManager::~SubscriberManager() = default;

SubscriberManager::SubscriberManager(SubscriberManager&& other) noexcept
    : streamRegistry_(std::move(other.streamRegistry_))
    , subscribers_(std::move(other.subscribers_))
    , streamSubscribers_(std::move(other.streamSubscribers_))
    , onSubscriberAddedCallback_(std::move(other.onSubscriberAddedCallback_))
    , onSubscriberRemovedCallback_(std::move(other.onSubscriberRemovedCallback_))
    , onFrameDroppedCallback_(std::move(other.onFrameDroppedCallback_))
{
}

SubscriberManager& SubscriberManager::operator=(SubscriberManager&& other) noexcept {
    if (this != &other) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        streamRegistry_ = std::move(other.streamRegistry_);
        subscribers_ = std::move(other.subscribers_);
        streamSubscribers_ = std::move(other.streamSubscribers_);

        std::lock_guard<std::mutex> callbackLock(callbackMutex_);
        onSubscriberAddedCallback_ = std::move(other.onSubscriberAddedCallback_);
        onSubscriberRemovedCallback_ = std::move(other.onSubscriberRemovedCallback_);
        onFrameDroppedCallback_ = std::move(other.onFrameDroppedCallback_);
    }
    return *this;
}

// =============================================================================
// Subscriber Lifecycle
// =============================================================================

core::Result<void, SubscriberManagerError> SubscriberManager::addSubscriber(
    const StreamKey& streamKey,
    SubscriberId subscriberId
) {
    return addSubscriber(streamKey, subscriberId, SubscriberConfig{});
}

core::Result<void, SubscriberManagerError> SubscriberManager::addSubscriber(
    const StreamKey& streamKey,
    SubscriberId subscriberId,
    const SubscriberConfig& config
) {
    // Check if stream exists in registry
    if (streamRegistry_ && !streamRegistry_->hasStream(streamKey)) {
        return core::Result<void, SubscriberManagerError>::error(
            SubscriberManagerError{
                SubscriberManagerError::Code::StreamNotFound,
                "Stream not found: " + streamKey.toString()
            }
        );
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Check if subscriber already exists
    if (subscribers_.find(subscriberId) != subscribers_.end()) {
        return core::Result<void, SubscriberManagerError>::error(
            SubscriberManagerError{
                SubscriberManagerError::Code::SubscriberAlreadyExists,
                "Subscriber already exists: " + std::to_string(subscriberId)
            }
        );
    }

    // Create subscriber state
    SubscriberState state;
    state.subscriberId = subscriberId;
    state.streamKey = streamKey;
    state.config = config;
    state.buffer = std::make_unique<SubscriberBuffer>(toBufferConfig(config));
    state.subscribedAt = std::chrono::steady_clock::now();
    state.lastDeliveryAt = state.subscribedAt;

    // Setup drop callback for the buffer
    setupDropCallback(state);

    // Add to subscribers map
    subscribers_.emplace(subscriberId, std::move(state));

    // Add to stream index
    streamSubscribers_[streamKey].insert(subscriberId);

    lock.unlock();

    // Emit callback
    emitSubscriberAdded(subscriberId, streamKey);

    return core::Result<void, SubscriberManagerError>::success();
}

core::Result<void, SubscriberManagerError> SubscriberManager::removeSubscriber(
    SubscriberId subscriberId
) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = subscribers_.find(subscriberId);
    if (it == subscribers_.end()) {
        return core::Result<void, SubscriberManagerError>::error(
            SubscriberManagerError{
                SubscriberManagerError::Code::SubscriberNotFound,
                "Subscriber not found: " + std::to_string(subscriberId)
            }
        );
    }

    StreamKey streamKey = it->second.streamKey;

    // Remove from stream index
    auto streamIt = streamSubscribers_.find(streamKey);
    if (streamIt != streamSubscribers_.end()) {
        streamIt->second.erase(subscriberId);
        if (streamIt->second.empty()) {
            streamSubscribers_.erase(streamIt);
        }
    }

    // Remove from subscribers map
    subscribers_.erase(it);

    lock.unlock();

    // Emit callback
    emitSubscriberRemoved(subscriberId, streamKey);

    return core::Result<void, SubscriberManagerError>::success();
}

void SubscriberManager::onSubscriberDisconnect(SubscriberId subscriberId) {
    // Simply remove the subscriber - ignore errors if not found
    removeSubscriber(subscriberId);
}

void SubscriberManager::onStreamEnded(const StreamKey& streamKey) {
    std::vector<SubscriberId> subscribersToRemove;

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        auto streamIt = streamSubscribers_.find(streamKey);
        if (streamIt != streamSubscribers_.end()) {
            // Copy subscriber IDs to remove
            subscribersToRemove.assign(
                streamIt->second.begin(),
                streamIt->second.end()
            );
        }
    }

    // Remove each subscriber (outside of lock to avoid callback deadlock)
    for (SubscriberId subId : subscribersToRemove) {
        removeSubscriber(subId);
    }
}

// =============================================================================
// Subscriber Queries
// =============================================================================

bool SubscriberManager::hasSubscriber(SubscriberId subscriberId) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return subscribers_.find(subscriberId) != subscribers_.end();
}

std::optional<StreamKey> SubscriberManager::getSubscriberStreamKey(
    SubscriberId subscriberId
) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = subscribers_.find(subscriberId);
    if (it == subscribers_.end()) {
        return std::nullopt;
    }

    return it->second.streamKey;
}

std::set<SubscriberId> SubscriberManager::getSubscribersForStream(
    const StreamKey& streamKey
) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = streamSubscribers_.find(streamKey);
    if (it == streamSubscribers_.end()) {
        return {};
    }

    return it->second;
}

size_t SubscriberManager::getSubscriberCount(const StreamKey& streamKey) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = streamSubscribers_.find(streamKey);
    if (it == streamSubscribers_.end()) {
        return 0;
    }

    return it->second.size();
}

size_t SubscriberManager::getTotalSubscriberCount() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return subscribers_.size();
}

// =============================================================================
// Buffer Operations
// =============================================================================

void SubscriberManager::pushToSubscriber(
    SubscriberId subscriberId,
    const BufferedFrame& frame
) {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = subscribers_.find(subscriberId);
    if (it == subscribers_.end()) {
        return;
    }

    it->second.buffer->push(frame);
}

std::optional<BufferedFrame> SubscriberManager::popFromSubscriber(
    SubscriberId subscriberId
) {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = subscribers_.find(subscriberId);
    if (it == subscribers_.end()) {
        return std::nullopt;
    }

    auto frame = it->second.buffer->pop();
    if (frame.has_value()) {
        // Update delivery statistics
        // Note: Using const_cast here is safe because we're modifying
        // non-const members of the SubscriberState
        auto& state = const_cast<SubscriberState&>(it->second);
        state.bytesDelivered += frame->data.size();
        state.framesDelivered++;
        state.lastDeliveryAt = std::chrono::steady_clock::now();
    }

    return frame;
}

void SubscriberManager::distributeToStream(
    const StreamKey& streamKey,
    const BufferedFrame& frame
) {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto streamIt = streamSubscribers_.find(streamKey);
    if (streamIt == streamSubscribers_.end()) {
        return;
    }

    for (SubscriberId subId : streamIt->second) {
        auto subIt = subscribers_.find(subId);
        if (subIt != subscribers_.end()) {
            subIt->second.buffer->push(frame);
        }
    }
}

// =============================================================================
// Statistics
// =============================================================================

std::optional<SubscriberStatistics> SubscriberManager::getSubscriberStats(
    SubscriberId subscriberId
) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = subscribers_.find(subscriberId);
    if (it == subscribers_.end()) {
        return std::nullopt;
    }

    const auto& state = it->second;
    auto bufferStats = state.buffer->getStatistics();

    SubscriberStatistics stats;
    stats.subscriberId = subscriberId;
    stats.streamKey = state.streamKey;
    stats.pendingFrames = state.buffer->getPendingFrameCount();
    stats.pendingBytes = state.buffer->getBufferedBytes();
    stats.bufferLevel = state.buffer->getBufferLevel();
    stats.bytesDelivered = state.bytesDelivered;
    stats.framesDelivered = state.framesDelivered;
    stats.droppedFrames = bufferStats.droppedInterFrames +
                          bufferStats.droppedKeyframes +
                          bufferStats.droppedAudioFrames;
    stats.droppedBytes = bufferStats.totalDroppedBytes;
    stats.subscribedAt = state.subscribedAt;
    stats.lastDeliveryAt = state.lastDeliveryAt;

    return stats;
}

std::vector<SubscriberStatistics> SubscriberManager::getAllSubscriberStats(
    const StreamKey& streamKey
) const {
    std::vector<SubscriberStatistics> allStats;

    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto streamIt = streamSubscribers_.find(streamKey);
    if (streamIt == streamSubscribers_.end()) {
        return allStats;
    }

    for (SubscriberId subId : streamIt->second) {
        auto subIt = subscribers_.find(subId);
        if (subIt != subscribers_.end()) {
            const auto& state = subIt->second;
            auto bufferStats = state.buffer->getStatistics();

            SubscriberStatistics stats;
            stats.subscriberId = subId;
            stats.streamKey = state.streamKey;
            stats.pendingFrames = state.buffer->getPendingFrameCount();
            stats.pendingBytes = state.buffer->getBufferedBytes();
            stats.bufferLevel = state.buffer->getBufferLevel();
            stats.bytesDelivered = state.bytesDelivered;
            stats.framesDelivered = state.framesDelivered;
            stats.droppedFrames = bufferStats.droppedInterFrames +
                                  bufferStats.droppedKeyframes +
                                  bufferStats.droppedAudioFrames;
            stats.droppedBytes = bufferStats.totalDroppedBytes;
            stats.subscribedAt = state.subscribedAt;
            stats.lastDeliveryAt = state.lastDeliveryAt;

            allStats.push_back(stats);
        }
    }

    return allStats;
}

std::optional<std::chrono::milliseconds> SubscriberManager::getSubscriberBufferLevel(
    SubscriberId subscriberId
) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = subscribers_.find(subscriberId);
    if (it == subscribers_.end()) {
        return std::nullopt;
    }

    return it->second.buffer->getBufferLevel();
}

// =============================================================================
// Configuration
// =============================================================================

std::optional<SubscriberConfig> SubscriberManager::getSubscriberConfig(
    SubscriberId subscriberId
) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = subscribers_.find(subscriberId);
    if (it == subscribers_.end()) {
        return std::nullopt;
    }

    return it->second.config;
}

core::Result<void, SubscriberManagerError> SubscriberManager::setSubscriberConfig(
    SubscriberId subscriberId,
    const SubscriberConfig& config
) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = subscribers_.find(subscriberId);
    if (it == subscribers_.end()) {
        return core::Result<void, SubscriberManagerError>::error(
            SubscriberManagerError{
                SubscriberManagerError::Code::SubscriberNotFound,
                "Subscriber not found: " + std::to_string(subscriberId)
            }
        );
    }

    it->second.config = config;
    it->second.buffer->setConfig(toBufferConfig(config));

    return core::Result<void, SubscriberManagerError>::success();
}

// =============================================================================
// Callbacks
// =============================================================================

void SubscriberManager::setOnSubscriberAddedCallback(OnSubscriberAddedCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    onSubscriberAddedCallback_ = std::move(callback);
}

void SubscriberManager::setOnSubscriberRemovedCallback(OnSubscriberRemovedCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    onSubscriberRemovedCallback_ = std::move(callback);
}

void SubscriberManager::setOnFrameDroppedCallback(OnSubscriberFrameDroppedCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    onFrameDroppedCallback_ = std::move(callback);

    // Update all existing subscriber buffers with the new callback
    std::unique_lock<std::shared_mutex> stateLock(mutex_);
    for (auto& pair : subscribers_) {
        setupDropCallback(pair.second);
    }
}

// =============================================================================
// Lifecycle
// =============================================================================

void SubscriberManager::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    subscribers_.clear();
    streamSubscribers_.clear();
}

// =============================================================================
// Private Methods
// =============================================================================

SubscriberBufferConfig SubscriberManager::toBufferConfig(const SubscriberConfig& config) const {
    SubscriberBufferConfig bufferConfig;
    bufferConfig.maxBufferDuration = config.maxBufferDuration;
    bufferConfig.maxBufferSize = config.maxBufferSize;
    bufferConfig.preserveAudio = config.preserveAudio;
    return bufferConfig;
}

void SubscriberManager::emitSubscriberAdded(SubscriberId subscriberId, const StreamKey& streamKey) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (onSubscriberAddedCallback_) {
        onSubscriberAddedCallback_(subscriberId, streamKey);
    }
}

void SubscriberManager::emitSubscriberRemoved(SubscriberId subscriberId, const StreamKey& streamKey) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (onSubscriberRemovedCallback_) {
        onSubscriberRemovedCallback_(subscriberId, streamKey);
    }
}

void SubscriberManager::setupDropCallback(SubscriberState& state) {
    // Capture subscriberId for the callback
    SubscriberId subId = state.subscriberId;

    state.buffer->setOnFrameDroppedCallback(
        [this, subId](const BufferedFrame& frame) {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            if (onFrameDroppedCallback_) {
                onFrameDroppedCallback_(subId, frame);
            }
        }
    );
}

} // namespace streaming
} // namespace openrtmp
