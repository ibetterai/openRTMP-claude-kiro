// OpenRTMP - Cross-platform RTMP Server
// Media Distribution Engine Implementation
//
// Thread-safe implementation of media distribution with:
// - Metadata and sequence header caching via GOP buffer
// - Instant playback from most recent keyframe (Requirement 5.1)
// - Cached data sent before stream data (Requirement 5.3)
// - EOF notification within 1 second (Requirement 5.6)
// - Slow subscriber detection and frame dropping
//
// Requirements coverage:
// - Requirement 5.1: Transmit data starting from most recent keyframe
// - Requirement 5.3: Send cached metadata and sequence headers before stream data
// - Requirement 5.6: Send stream EOF message within 1 second of stream end

#include "openrtmp/streaming/media_distribution.hpp"

namespace openrtmp {
namespace streaming {

// =============================================================================
// Constructor / Destructor
// =============================================================================

MediaDistribution::MediaDistribution(
    std::shared_ptr<IStreamRegistry> streamRegistry,
    std::shared_ptr<ISubscriberManager> subscriberManager
)
    : streamRegistry_(std::move(streamRegistry))
    , subscriberManager_(std::move(subscriberManager))
{
    // Setup frame dropped callback to detect slow subscribers
    if (subscriberManager_) {
        subscriberManager_->setOnFrameDroppedCallback(
            [this](SubscriberId subId, const BufferedFrame&) {
                emitSlowSubscriber(subId);
            }
        );
    }
}

MediaDistribution::~MediaDistribution() = default;

MediaDistribution::MediaDistribution(MediaDistribution&& other) noexcept
    : streamRegistry_(std::move(other.streamRegistry_))
    , subscriberManager_(std::move(other.subscriberManager_))
    , streamStates_(std::move(other.streamStates_))
    , onFrameSentCallback_(std::move(other.onFrameSentCallback_))
    , onStreamEOFCallback_(std::move(other.onStreamEOFCallback_))
    , onSlowSubscriberCallback_(std::move(other.onSlowSubscriberCallback_))
    , slowSubscriberThreshold_(other.slowSubscriberThreshold_)
{
}

MediaDistribution& MediaDistribution::operator=(MediaDistribution&& other) noexcept {
    if (this != &other) {
        std::unique_lock<std::shared_mutex> lock(streamStatesMutex_);
        streamRegistry_ = std::move(other.streamRegistry_);
        subscriberManager_ = std::move(other.subscriberManager_);
        streamStates_ = std::move(other.streamStates_);

        std::lock_guard<std::mutex> callbackLock(callbackMutex_);
        onFrameSentCallback_ = std::move(other.onFrameSentCallback_);
        onStreamEOFCallback_ = std::move(other.onStreamEOFCallback_);
        onSlowSubscriberCallback_ = std::move(other.onSlowSubscriberCallback_);
        slowSubscriberThreshold_ = other.slowSubscriberThreshold_;
    }
    return *this;
}

// =============================================================================
// GOP Buffer Management
// =============================================================================

void MediaDistribution::setGOPBuffer(
    const StreamKey& streamKey,
    std::shared_ptr<IGOPBuffer> buffer
) {
    std::unique_lock<std::shared_mutex> lock(streamStatesMutex_);

    auto& state = streamStates_[streamKey];
    state.streamKey = streamKey;
    state.gopBuffer = std::move(buffer);
    state.startedAt = std::chrono::steady_clock::now();
}

std::shared_ptr<IGOPBuffer> MediaDistribution::getGOPBuffer(
    const StreamKey& streamKey
) const {
    std::shared_lock<std::shared_mutex> lock(streamStatesMutex_);

    auto it = streamStates_.find(streamKey);
    if (it == streamStates_.end()) {
        return nullptr;
    }
    return it->second.gopBuffer;
}

// =============================================================================
// Subscriber Management
// =============================================================================

core::Result<void, MediaDistributionError> MediaDistribution::addSubscriber(
    const StreamKey& streamKey,
    SubscriberId subscriberId
) {
    return addSubscriber(streamKey, subscriberId, SubscriberConfig{});
}

core::Result<void, MediaDistributionError> MediaDistribution::addSubscriber(
    const StreamKey& streamKey,
    SubscriberId subscriberId,
    const SubscriberConfig& config
) {
    // Check if stream exists
    if (streamRegistry_ && !streamRegistry_->hasStream(streamKey)) {
        return core::Result<void, MediaDistributionError>::error(
            MediaDistributionError{
                MediaDistributionError::Code::StreamNotFound,
                "Stream not found: " + streamKey.toString()
            }
        );
    }

    // Add subscriber via subscriber manager
    auto result = subscriberManager_->addSubscriber(streamKey, subscriberId, config);
    if (!result.isSuccess()) {
        // Map SubscriberManagerError to MediaDistributionError
        auto& err = result.error();
        MediaDistributionError::Code code = MediaDistributionError::Code::InternalError;
        if (err.code == SubscriberManagerError::Code::StreamNotFound) {
            code = MediaDistributionError::Code::StreamNotFound;
        } else if (err.code == SubscriberManagerError::Code::SubscriberAlreadyExists) {
            code = MediaDistributionError::Code::SubscriberAlreadyExists;
        }
        return core::Result<void, MediaDistributionError>::error(
            MediaDistributionError{code, err.message}
        );
    }

    // Get GOP buffer and send cached data
    std::shared_ptr<IGOPBuffer> gopBuffer;
    {
        std::shared_lock<std::shared_mutex> lock(streamStatesMutex_);
        auto it = streamStates_.find(streamKey);
        if (it != streamStates_.end()) {
            gopBuffer = it->second.gopBuffer;
        }
    }

    if (gopBuffer) {
        sendCachedDataToSubscriber(subscriberId, gopBuffer);
    }

    return core::Result<void, MediaDistributionError>::success();
}

core::Result<void, MediaDistributionError> MediaDistribution::removeSubscriber(
    SubscriberId subscriberId
) {
    auto result = subscriberManager_->removeSubscriber(subscriberId);
    if (!result.isSuccess()) {
        return core::Result<void, MediaDistributionError>::error(
            MediaDistributionError{
                MediaDistributionError::Code::SubscriberNotFound,
                result.error().message
            }
        );
    }
    return core::Result<void, MediaDistributionError>::success();
}

bool MediaDistribution::hasSubscriber(SubscriberId subscriberId) const {
    return subscriberManager_->hasSubscriber(subscriberId);
}

size_t MediaDistribution::getSubscriberCount(const StreamKey& streamKey) const {
    return subscriberManager_->getSubscriberCount(streamKey);
}

// =============================================================================
// Media Distribution
// =============================================================================

void MediaDistribution::distribute(
    const StreamKey& streamKey,
    const BufferedFrame& frame
) {
    // Get GOP buffer and update it
    std::shared_ptr<IGOPBuffer> gopBuffer;
    {
        std::shared_lock<std::shared_mutex> lock(streamStatesMutex_);
        auto it = streamStates_.find(streamKey);
        if (it != streamStates_.end()) {
            gopBuffer = it->second.gopBuffer;
        }
    }

    // Push to GOP buffer for caching
    if (gopBuffer) {
        gopBuffer->push(frame);
    }

    // Get all subscribers for this stream
    auto subscribers = subscriberManager_->getSubscribersForStream(streamKey);
    if (subscribers.empty()) {
        return;
    }

    // Distribute to all subscribers
    for (SubscriberId subId : subscribers) {
        // Check for slow subscriber
        checkSlowSubscriber(subId);

        // Send frame
        sendFrameToSubscriber(subId, frame);
    }

    // Update statistics
    {
        std::unique_lock<std::shared_mutex> lock(streamStatesMutex_);
        auto it = streamStates_.find(streamKey);
        if (it != streamStates_.end()) {
            it->second.totalFramesDistributed += subscribers.size();
            it->second.totalBytesDistributed += frame.data.size() * subscribers.size();
        }
    }
}

void MediaDistribution::setMetadata(
    const StreamKey& streamKey,
    const protocol::AMFValue& metadata
) {
    std::shared_lock<std::shared_mutex> lock(streamStatesMutex_);
    auto it = streamStates_.find(streamKey);
    if (it != streamStates_.end() && it->second.gopBuffer) {
        it->second.gopBuffer->setMetadata(metadata);
    }
}

void MediaDistribution::setSequenceHeaders(
    const StreamKey& streamKey,
    const std::vector<uint8_t>& videoHeader,
    const std::vector<uint8_t>& audioHeader
) {
    std::shared_lock<std::shared_mutex> lock(streamStatesMutex_);
    auto it = streamStates_.find(streamKey);
    if (it != streamStates_.end() && it->second.gopBuffer) {
        it->second.gopBuffer->setSequenceHeaders(videoHeader, audioHeader);
    }
}

// =============================================================================
// Stream Lifecycle
// =============================================================================

void MediaDistribution::onStreamEnd(const StreamKey& streamKey) {
    // Get all subscribers for this stream
    auto subscribers = subscriberManager_->getSubscribersForStream(streamKey);

    // Send EOF to all subscribers immediately (within 1 second per Requirement 5.6)
    for (SubscriberId subId : subscribers) {
        emitStreamEOF(subId, streamKey);
    }

    // Notify subscriber manager that stream ended
    subscriberManager_->onStreamEnded(streamKey);
}

// =============================================================================
// Statistics
// =============================================================================

std::optional<DistributionStats> MediaDistribution::getDistributionStats(
    const StreamKey& streamKey
) const {
    std::shared_lock<std::shared_mutex> lock(streamStatesMutex_);

    auto it = streamStates_.find(streamKey);
    if (it == streamStates_.end()) {
        return std::nullopt;
    }

    const auto& state = it->second;
    DistributionStats stats;
    stats.streamKey = streamKey;
    stats.subscriberCount = subscriberManager_->getSubscriberCount(streamKey);
    stats.totalFramesDistributed = state.totalFramesDistributed;
    stats.totalBytesDistributed = state.totalBytesDistributed;
    stats.droppedFrames = state.droppedFrames;
    stats.startedAt = state.startedAt;

    return stats;
}

// =============================================================================
// Callbacks
// =============================================================================

void MediaDistribution::setOnFrameSentCallback(OnFrameSentCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    onFrameSentCallback_ = std::move(callback);
}

void MediaDistribution::setOnStreamEOFCallback(OnStreamEOFCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    onStreamEOFCallback_ = std::move(callback);
}

void MediaDistribution::setOnSlowSubscriberCallback(OnSlowSubscriberCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    onSlowSubscriberCallback_ = std::move(callback);
}

// =============================================================================
// Private Methods
// =============================================================================

void MediaDistribution::sendCachedDataToSubscriber(
    SubscriberId subscriberId,
    const std::shared_ptr<IGOPBuffer>& gopBuffer
) {
    if (!gopBuffer) {
        return;
    }

    // 1. Send metadata first (Requirement 5.3)
    auto metadata = gopBuffer->getMetadata();
    if (metadata.has_value()) {
        // Create a data frame for metadata
        BufferedFrame metadataFrame;
        metadataFrame.type = MediaType::Data;
        metadataFrame.timestamp = 0;
        metadataFrame.isKeyframe = false;
        // In real implementation, this would be AMF-encoded metadata
        // For now, use a placeholder
        metadataFrame.data = {0x02}; // AMF0 string marker

        sendFrameToSubscriber(subscriberId, metadataFrame);
    }

    // 2. Send sequence headers (Requirement 5.3)
    auto headers = gopBuffer->getSequenceHeaders();
    if (!headers.videoHeader.empty()) {
        BufferedFrame videoHeaderFrame;
        videoHeaderFrame.type = MediaType::Video;
        videoHeaderFrame.timestamp = 0;
        videoHeaderFrame.isKeyframe = true;  // Sequence header treated as keyframe
        videoHeaderFrame.data = headers.videoHeader;

        sendFrameToSubscriber(subscriberId, videoHeaderFrame);
    }
    if (!headers.audioHeader.empty()) {
        BufferedFrame audioHeaderFrame;
        audioHeaderFrame.type = MediaType::Audio;
        audioHeaderFrame.timestamp = 0;
        audioHeaderFrame.isKeyframe = false;
        audioHeaderFrame.data = headers.audioHeader;

        sendFrameToSubscriber(subscriberId, audioHeaderFrame);
    }

    // 3. Send frames from most recent keyframe (Requirement 5.1)
    auto cachedFrames = gopBuffer->getFromLastKeyframe();
    for (const auto& frame : cachedFrames) {
        sendFrameToSubscriber(subscriberId, frame);
    }
}

void MediaDistribution::sendFrameToSubscriber(
    SubscriberId subscriberId,
    const BufferedFrame& frame
) {
    // Push to subscriber's buffer via subscriber manager
    subscriberManager_->pushToSubscriber(subscriberId, frame);

    // Emit callback
    emitFrameSent(subscriberId, frame);
}

bool MediaDistribution::checkSlowSubscriber(SubscriberId subscriberId) {
    auto bufferLevel = subscriberManager_->getSubscriberBufferLevel(subscriberId);
    if (!bufferLevel.has_value()) {
        return false;
    }

    if (*bufferLevel > slowSubscriberThreshold_) {
        emitSlowSubscriber(subscriberId);
        return true;
    }

    return false;
}

void MediaDistribution::emitFrameSent(SubscriberId subscriberId, const BufferedFrame& frame) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (onFrameSentCallback_) {
        onFrameSentCallback_(subscriberId, frame);
    }
}

void MediaDistribution::emitStreamEOF(SubscriberId subscriberId, const StreamKey& streamKey) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (onStreamEOFCallback_) {
        onStreamEOFCallback_(subscriberId, streamKey);
    }
}

void MediaDistribution::emitSlowSubscriber(SubscriberId subscriberId) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (onSlowSubscriberCallback_) {
        onSlowSubscriberCallback_(subscriberId);
    }
}

} // namespace streaming
} // namespace openrtmp
