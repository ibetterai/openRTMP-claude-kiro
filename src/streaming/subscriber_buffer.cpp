// OpenRTMP - Cross-platform RTMP Server
// Subscriber Buffer Implementation
//
// Provides per-subscriber buffer management with overflow protection:
// - Track per-subscriber buffer levels independently
// - Drop non-keyframe packets when buffer exceeds configured threshold
// - Preserve keyframes and audio to maintain stream continuity
// - Log dropped frame statistics per subscriber
// - Support configurable maximum buffer thresholds
//
// Requirements coverage:
// - Requirement 5.4: Maintain independent send buffers for each subscriber
// - Requirement 5.5: Drop non-keyframe packets if buffer exceeds 5 seconds

#include "openrtmp/streaming/subscriber_buffer.hpp"

#include <algorithm>
#include <limits>

namespace openrtmp {
namespace streaming {

// =============================================================================
// SubscriberBuffer Implementation
// =============================================================================

// -----------------------------------------------------------------------------
// Constructor / Destructor
// -----------------------------------------------------------------------------

SubscriberBuffer::SubscriberBuffer()
    : config_()
{
}

SubscriberBuffer::SubscriberBuffer(const SubscriberBufferConfig& config)
    : config_(config)
{
}

SubscriberBuffer::SubscriberBuffer(SubscriberBuffer&& other) noexcept
    : frames_(std::move(other.frames_))
    , bufferedBytes_(other.bufferedBytes_)
    , statistics_(std::move(other.statistics_))
    , config_(std::move(other.config_))
    , onFrameDroppedCallback_(std::move(other.onFrameDroppedCallback_))
{
    other.bufferedBytes_ = 0;
}

SubscriberBuffer& SubscriberBuffer::operator=(SubscriberBuffer&& other) noexcept {
    if (this != &other) {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_ = std::move(other.frames_);
        bufferedBytes_ = other.bufferedBytes_;
        statistics_ = std::move(other.statistics_);
        config_ = std::move(other.config_);
        onFrameDroppedCallback_ = std::move(other.onFrameDroppedCallback_);

        other.bufferedBytes_ = 0;
    }
    return *this;
}

// -----------------------------------------------------------------------------
// Frame Management
// -----------------------------------------------------------------------------

void SubscriberBuffer::push(const BufferedFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Update statistics
    statistics_.totalFramesProcessed++;
    statistics_.totalBytesProcessed += frame.data.size();

    // Check if we should drop this frame due to buffer overflow
    if (shouldDropFrame(frame)) {
        recordDroppedFrame(frame);

        // Invoke callback if registered
        if (onFrameDroppedCallback_) {
            // Note: Callback is called with mutex held
            // Consider if this needs to be changed for performance
            onFrameDroppedCallback_(frame);
        }
        return;
    }

    // Add frame to buffer
    frames_.push_back(frame);
    bufferedBytes_ += frame.data.size();

    // Enforce buffer limit after adding (drop oldest non-keyframes if needed)
    enforceBufferLimit();
}

std::optional<BufferedFrame> SubscriberBuffer::pop() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (frames_.empty()) {
        return std::nullopt;
    }

    BufferedFrame frame = std::move(frames_.front());
    frames_.pop_front();
    bufferedBytes_ -= frame.data.size();

    return frame;
}

std::optional<BufferedFrame> SubscriberBuffer::peek() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (frames_.empty()) {
        return std::nullopt;
    }

    return frames_.front();
}

// -----------------------------------------------------------------------------
// Buffer Status
// -----------------------------------------------------------------------------

std::chrono::milliseconds SubscriberBuffer::getBufferLevel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calculateBufferDuration();
}

size_t SubscriberBuffer::getBufferedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bufferedBytes_;
}

size_t SubscriberBuffer::getPendingFrameCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size();
}

bool SubscriberBuffer::isEmpty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.empty();
}

// -----------------------------------------------------------------------------
// Statistics
// -----------------------------------------------------------------------------

uint64_t SubscriberBuffer::getDroppedFrameCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return statistics_.droppedInterFrames + statistics_.droppedKeyframes + statistics_.droppedAudioFrames;
}

SubscriberBufferStatistics SubscriberBuffer::getStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return statistics_;
}

void SubscriberBuffer::resetStatistics() {
    std::lock_guard<std::mutex> lock(mutex_);
    statistics_ = SubscriberBufferStatistics{};
}

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

void SubscriberBuffer::setConfig(const SubscriberBufferConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;

    // Apply new constraints immediately
    enforceBufferLimit();
}

SubscriberBufferConfig SubscriberBuffer::getConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

// -----------------------------------------------------------------------------
// Callbacks
// -----------------------------------------------------------------------------

void SubscriberBuffer::setOnFrameDroppedCallback(OnFrameDroppedCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    onFrameDroppedCallback_ = std::move(callback);
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

void SubscriberBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.clear();
    bufferedBytes_ = 0;
    // Statistics are preserved
}

void SubscriberBuffer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.clear();
    bufferedBytes_ = 0;
    statistics_ = SubscriberBufferStatistics{};
}

// -----------------------------------------------------------------------------
// Private Helpers
// -----------------------------------------------------------------------------

bool SubscriberBuffer::shouldDropFrame(const BufferedFrame& frame) const {
    // This is called with mutex already held

    // Never drop keyframes - they are essential for stream continuity
    if (frame.type == MediaType::Video && frame.isKeyframe) {
        return false;
    }

    // Check if preserving audio and this is an audio frame
    if (config_.preserveAudio && frame.type == MediaType::Audio) {
        return false;
    }

    // Check buffer duration threshold
    auto bufferDuration = calculateBufferDuration();

    // If buffer exceeds threshold, drop non-essential frames
    if (bufferDuration > config_.maxBufferDuration) {
        // This is a video inter frame (P/B frame) - candidate for dropping
        if (frame.type == MediaType::Video && !frame.isKeyframe) {
            return true;
        }
    }

    // Check size limit if configured
    if (config_.maxBufferSize > 0 && bufferedBytes_ > config_.maxBufferSize) {
        if (frame.type == MediaType::Video && !frame.isKeyframe) {
            return true;
        }
    }

    return false;
}

void SubscriberBuffer::enforceBufferLimit() {
    // This is called with mutex already held

    if (frames_.empty()) {
        return;
    }

    auto bufferDuration = calculateBufferDuration();

    // While buffer exceeds limit, drop frames aggressively
    // We need to drop ALL droppable frames in one pass to bring buffer below threshold
    while (bufferDuration > config_.maxBufferDuration && frames_.size() > 1) {
        // Collect indices of droppable frames
        std::vector<std::deque<BufferedFrame>::iterator> droppableFrames;

        for (auto it = frames_.begin(); it != frames_.end(); ++it) {
            const auto& frame = *it;

            // Skip keyframes - always preserve
            if (frame.type == MediaType::Video && frame.isKeyframe) {
                continue;
            }

            // Skip audio if preserving
            if (config_.preserveAudio && frame.type == MediaType::Audio) {
                continue;
            }

            // Skip data frames - typically metadata, should preserve
            if (frame.type == MediaType::Data) {
                continue;
            }

            // This is a droppable frame (video inter frame)
            droppableFrames.push_back(it);
        }

        // If no droppable frames found, stop trying
        if (droppableFrames.empty()) {
            break;
        }

        // Drop frames from oldest to newest until buffer is within limit
        // We drop one frame at a time and recheck (iterators invalidate on erase)
        bool droppedAny = false;
        for (auto it = frames_.begin(); it != frames_.end(); ) {
            const auto& frame = *it;

            // Check if we're within limit now
            bufferDuration = calculateBufferDuration();
            if (bufferDuration <= config_.maxBufferDuration) {
                break;
            }

            // Skip keyframes - always preserve
            if (frame.type == MediaType::Video && frame.isKeyframe) {
                ++it;
                continue;
            }

            // Skip audio if preserving
            if (config_.preserveAudio && frame.type == MediaType::Audio) {
                ++it;
                continue;
            }

            // Skip data frames
            if (frame.type == MediaType::Data) {
                ++it;
                continue;
            }

            // Drop this frame (video inter frame)
            recordDroppedFrame(*it);

            // Invoke callback
            if (onFrameDroppedCallback_) {
                onFrameDroppedCallback_(*it);
            }

            bufferedBytes_ -= it->data.size();
            it = frames_.erase(it);
            droppedAny = true;
        }

        // If we didn't drop anything in this pass, break to avoid infinite loop
        if (!droppedAny) {
            break;
        }

        // Recalculate duration
        bufferDuration = calculateBufferDuration();
    }
}

std::chrono::milliseconds SubscriberBuffer::calculateBufferDuration() const {
    // This is called with mutex already held

    if (frames_.empty()) {
        return std::chrono::milliseconds(0);
    }

    // Find min and max timestamps
    uint32_t minTs = std::numeric_limits<uint32_t>::max();
    uint32_t maxTs = 0;

    for (const auto& frame : frames_) {
        if (frame.timestamp < minTs) {
            minTs = frame.timestamp;
        }
        if (frame.timestamp > maxTs) {
            maxTs = frame.timestamp;
        }
    }

    // Handle normal case
    if (maxTs >= minTs) {
        return std::chrono::milliseconds(maxTs - minTs);
    }

    // Timestamp wraparound - return 0 as safe default
    return std::chrono::milliseconds(0);
}

void SubscriberBuffer::recordDroppedFrame(const BufferedFrame& frame) {
    // This is called with mutex already held

    statistics_.totalDroppedBytes += frame.data.size();
    statistics_.lastDropTime = std::chrono::steady_clock::now();

    if (frame.type == MediaType::Video) {
        if (frame.isKeyframe) {
            statistics_.droppedKeyframes++;
        } else {
            statistics_.droppedInterFrames++;
        }
    } else if (frame.type == MediaType::Audio) {
        statistics_.droppedAudioFrames++;
    }
}

// =============================================================================
// SubscriberBufferManager Implementation
// =============================================================================

// -----------------------------------------------------------------------------
// Subscriber Management
// -----------------------------------------------------------------------------

void SubscriberBufferManager::addSubscriber(SubscriberId subscriberId) {
    addSubscriber(subscriberId, defaultConfig_);
}

void SubscriberBufferManager::addSubscriber(SubscriberId subscriberId, const SubscriberBufferConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Create new buffer for this subscriber
    auto buffer = std::make_unique<SubscriberBuffer>(config);

    // Set up callback forwarding if global callback is registered
    if (onFrameDroppedCallback_) {
        SubscriberId subId = subscriberId;  // Capture by value
        buffer->setOnFrameDroppedCallback([this, subId](const BufferedFrame& frame) {
            // Forward to global callback with subscriber ID
            // Note: This callback is called from SubscriberBuffer with its mutex held
            // The outer mutex should not be acquired here to avoid deadlock
            if (onFrameDroppedCallback_) {
                onFrameDroppedCallback_(subId, frame);
            }
        });
    }

    buffers_[subscriberId] = std::move(buffer);
}

void SubscriberBufferManager::removeSubscriber(SubscriberId subscriberId) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffers_.erase(subscriberId);
}

bool SubscriberBufferManager::hasSubscriber(SubscriberId subscriberId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffers_.find(subscriberId) != buffers_.end();
}

std::vector<SubscriberId> SubscriberBufferManager::getSubscriberIds() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<SubscriberId> ids;
    ids.reserve(buffers_.size());

    for (const auto& pair : buffers_) {
        ids.push_back(pair.first);
    }

    return ids;
}

size_t SubscriberBufferManager::getSubscriberCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffers_.size();
}

// -----------------------------------------------------------------------------
// Frame Distribution
// -----------------------------------------------------------------------------

void SubscriberBufferManager::push(SubscriberId subscriberId, const BufferedFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = buffers_.find(subscriberId);
    if (it != buffers_.end()) {
        it->second->push(frame);
    }
}

void SubscriberBufferManager::distributeToAll(const BufferedFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& pair : buffers_) {
        pair.second->push(frame);
    }
}

std::optional<BufferedFrame> SubscriberBufferManager::pop(SubscriberId subscriberId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = buffers_.find(subscriberId);
    if (it != buffers_.end()) {
        return it->second->pop();
    }

    return std::nullopt;
}

// -----------------------------------------------------------------------------
// Statistics
// -----------------------------------------------------------------------------

SubscriberStats SubscriberBufferManager::getSubscriberStats(SubscriberId subscriberId) const {
    std::lock_guard<std::mutex> lock(mutex_);

    SubscriberStats stats;
    stats.subscriberId = subscriberId;

    auto it = buffers_.find(subscriberId);
    if (it != buffers_.end()) {
        const auto& buffer = it->second;
        stats.pendingFrames = buffer->getPendingFrameCount();
        stats.pendingBytes = buffer->getBufferedBytes();
        stats.bufferLevel = buffer->getBufferLevel();
        stats.droppedFrames = buffer->getDroppedFrameCount();

        auto detailedStats = buffer->getStatistics();
        stats.deliveredFrames = detailedStats.totalFramesProcessed - buffer->getDroppedFrameCount();
    }

    return stats;
}

std::vector<SubscriberStats> SubscriberBufferManager::getAllSubscriberStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<SubscriberStats> allStats;
    allStats.reserve(buffers_.size());

    for (const auto& pair : buffers_) {
        SubscriberStats stats;
        stats.subscriberId = pair.first;

        const auto& buffer = pair.second;
        stats.pendingFrames = buffer->getPendingFrameCount();
        stats.pendingBytes = buffer->getBufferedBytes();
        stats.bufferLevel = buffer->getBufferLevel();
        stats.droppedFrames = buffer->getDroppedFrameCount();

        auto detailedStats = buffer->getStatistics();
        stats.deliveredFrames = detailedStats.totalFramesProcessed - buffer->getDroppedFrameCount();

        allStats.push_back(stats);
    }

    return allStats;
}

uint64_t SubscriberBufferManager::getTotalDroppedFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t total = 0;
    for (const auto& pair : buffers_) {
        total += pair.second->getDroppedFrameCount();
    }

    return total;
}

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

void SubscriberBufferManager::setSubscriberConfig(SubscriberId subscriberId, const SubscriberBufferConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = buffers_.find(subscriberId);
    if (it != buffers_.end()) {
        it->second->setConfig(config);
    }
}

SubscriberBufferConfig SubscriberBufferManager::getSubscriberConfig(SubscriberId subscriberId) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = buffers_.find(subscriberId);
    if (it != buffers_.end()) {
        return it->second->getConfig();
    }

    return SubscriberBufferConfig{};
}

void SubscriberBufferManager::setDefaultConfig(const SubscriberBufferConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    defaultConfig_ = config;
}

// -----------------------------------------------------------------------------
// Callbacks
// -----------------------------------------------------------------------------

void SubscriberBufferManager::setOnFrameDroppedCallback(
    std::function<void(SubscriberId, const BufferedFrame&)> callback
) {
    std::lock_guard<std::mutex> lock(mutex_);
    onFrameDroppedCallback_ = std::move(callback);

    // Update existing subscribers with forwarding callback
    for (auto& pair : buffers_) {
        SubscriberId subId = pair.first;
        pair.second->setOnFrameDroppedCallback([this, subId](const BufferedFrame& frame) {
            if (onFrameDroppedCallback_) {
                onFrameDroppedCallback_(subId, frame);
            }
        });
    }
}

} // namespace streaming
} // namespace openrtmp
