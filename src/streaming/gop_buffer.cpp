// OpenRTMP - Cross-platform RTMP Server
// GOP Buffer Implementation

#include "openrtmp/streaming/gop_buffer.hpp"

#include <algorithm>
#include <limits>

namespace openrtmp {
namespace streaming {

// =============================================================================
// Constructor / Destructor
// =============================================================================

GOPBuffer::GOPBuffer()
    : lastKeyframeIndex_(0)
    , hasKeyframe_(false)
    , bufferedBytes_(0)
    , config_()
{
}

GOPBuffer::GOPBuffer(const GOPBufferConfig& config)
    : lastKeyframeIndex_(0)
    , hasKeyframe_(false)
    , bufferedBytes_(0)
    , config_(config)
{
}

GOPBuffer::GOPBuffer(GOPBuffer&& other) noexcept
    : frames_(std::move(other.frames_))
    , lastKeyframeIndex_(other.lastKeyframeIndex_)
    , hasKeyframe_(other.hasKeyframe_)
    , metadata_(std::move(other.metadata_))
    , sequenceHeaders_(std::move(other.sequenceHeaders_))
    , bufferedBytes_(other.bufferedBytes_)
    , config_(std::move(other.config_))
{
    other.lastKeyframeIndex_ = 0;
    other.hasKeyframe_ = false;
    other.bufferedBytes_ = 0;
}

GOPBuffer& GOPBuffer::operator=(GOPBuffer&& other) noexcept {
    if (this != &other) {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_ = std::move(other.frames_);
        lastKeyframeIndex_ = other.lastKeyframeIndex_;
        hasKeyframe_ = other.hasKeyframe_;
        metadata_ = std::move(other.metadata_);
        sequenceHeaders_ = std::move(other.sequenceHeaders_);
        bufferedBytes_ = other.bufferedBytes_;
        config_ = std::move(other.config_);

        other.lastKeyframeIndex_ = 0;
        other.hasKeyframe_ = false;
        other.bufferedBytes_ = 0;
    }
    return *this;
}

// =============================================================================
// Frame Management
// =============================================================================

void GOPBuffer::push(const BufferedFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Add frame to buffer
    frames_.push_back(frame);
    bufferedBytes_ += frame.data.size();

    // Update keyframe tracking
    if (frame.isKeyframe && frame.type == MediaType::Video) {
        lastKeyframeIndex_ = frames_.size() - 1;
        hasKeyframe_ = true;
    }

    // Evict old frames if necessary
    evictOldFrames();
}

// =============================================================================
// Metadata and Headers
// =============================================================================

void GOPBuffer::setMetadata(const protocol::AMFValue& metadata) {
    std::lock_guard<std::mutex> lock(mutex_);
    metadata_ = metadata;
}

void GOPBuffer::setSequenceHeaders(
    const std::vector<uint8_t>& videoHeader,
    const std::vector<uint8_t>& audioHeader
) {
    std::lock_guard<std::mutex> lock(mutex_);
    sequenceHeaders_.videoHeader = videoHeader;
    sequenceHeaders_.audioHeader = audioHeader;
}

// =============================================================================
// Retrieval
// =============================================================================

std::optional<protocol::AMFValue> GOPBuffer::getMetadata() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metadata_;
}

SequenceHeaders GOPBuffer::getSequenceHeaders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sequenceHeaders_;
}

std::vector<BufferedFrame> GOPBuffer::getFromLastKeyframe() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!hasKeyframe_ || frames_.empty()) {
        return {};
    }

    // Return frames from last keyframe to end
    std::vector<BufferedFrame> result;
    result.reserve(frames_.size() - lastKeyframeIndex_);

    for (size_t i = lastKeyframeIndex_; i < frames_.size(); ++i) {
        result.push_back(frames_[i]);
    }

    return result;
}

// =============================================================================
// Statistics
// =============================================================================

std::chrono::milliseconds GOPBuffer::getBufferedDuration() const {
    std::lock_guard<std::mutex> lock(mutex_);

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

    // Handle timestamp wraparound (though unlikely in practice)
    if (maxTs >= minTs) {
        return std::chrono::milliseconds(maxTs - minTs);
    }

    // Wraparound case: assume small duration
    return std::chrono::milliseconds(0);
}

size_t GOPBuffer::getBufferedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bufferedBytes_;
}

size_t GOPBuffer::getFrameCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size();
}

// =============================================================================
// Configuration
// =============================================================================

void GOPBuffer::setConfig(const GOPBufferConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;

    // Apply new constraints immediately
    evictOldFrames();
}

GOPBufferConfig GOPBuffer::getConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

// =============================================================================
// Lifecycle
// =============================================================================

void GOPBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    frames_.clear();
    bufferedBytes_ = 0;
    lastKeyframeIndex_ = 0;
    hasKeyframe_ = false;
    // Note: metadata_ and sequenceHeaders_ are preserved
}

void GOPBuffer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);

    frames_.clear();
    bufferedBytes_ = 0;
    lastKeyframeIndex_ = 0;
    hasKeyframe_ = false;
    metadata_.reset();
    sequenceHeaders_.videoHeader.clear();
    sequenceHeaders_.audioHeader.clear();
}

// =============================================================================
// Private Helpers
// =============================================================================

void GOPBuffer::evictOldFrames() {
    // This is called with mutex already held

    if (frames_.empty()) {
        return;
    }

    // Calculate current duration
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

    // Check if we need to evict based on size or duration constraints
    bool needsEviction = false;

    // Check buffer size constraint
    if (bufferedBytes_ > config_.maxBufferSize) {
        needsEviction = true;
    }

    // Check duration constraint - only evict if we have more than min duration
    // AND we need to (size limit or significantly over duration)
    auto currentDuration = std::chrono::milliseconds(maxTs >= minTs ? maxTs - minTs : 0);
    if (currentDuration > config_.minBufferDuration * 2) {
        needsEviction = true;
    }

    if (!needsEviction) {
        return;
    }

    // Find the second-to-last keyframe to use as eviction boundary
    // We want to keep at least one complete GOP
    size_t evictionBoundary = 0;
    size_t keyframeCount = 0;
    std::vector<size_t> keyframeIndices;

    for (size_t i = 0; i < frames_.size(); ++i) {
        if (frames_[i].isKeyframe && frames_[i].type == MediaType::Video) {
            keyframeIndices.push_back(i);
            ++keyframeCount;
        }
    }

    // We need at least 2 keyframes to evict the first GOP
    if (keyframeCount < 2) {
        // Just enforce size limit by evicting from front
        while (!frames_.empty() && bufferedBytes_ > config_.maxBufferSize) {
            bufferedBytes_ -= frames_.front().data.size();
            frames_.pop_front();
        }
        updateKeyframeIndex();
        return;
    }

    // Find how many GOPs we can evict while staying within constraints
    // We always keep at least one complete GOP (from last keyframe onwards)
    evictionBoundary = 0;

    // Try to evict oldest GOPs until we meet constraints
    for (size_t kfIdx = 0; kfIdx < keyframeIndices.size() - 1; ++kfIdx) {
        // Calculate what would remain if we evict up to this keyframe
        size_t proposedBoundary = keyframeIndices[kfIdx + 1];
        size_t proposedBytes = 0;
        uint32_t proposedMinTs = std::numeric_limits<uint32_t>::max();

        for (size_t i = proposedBoundary; i < frames_.size(); ++i) {
            proposedBytes += frames_[i].data.size();
            if (frames_[i].timestamp < proposedMinTs) {
                proposedMinTs = frames_[i].timestamp;
            }
        }

        auto proposedDuration = std::chrono::milliseconds(
            maxTs >= proposedMinTs ? maxTs - proposedMinTs : 0
        );

        // Check if evicting to this boundary meets min requirements
        if (proposedDuration >= config_.minBufferDuration ||
            (proposedBytes <= config_.maxBufferSize && kfIdx == keyframeIndices.size() - 2)) {
            evictionBoundary = proposedBoundary;

            // If we're within constraints, we can stop
            if (proposedBytes <= config_.maxBufferSize) {
                break;
            }
        }
    }

    // Perform eviction
    if (evictionBoundary > 0) {
        for (size_t i = 0; i < evictionBoundary; ++i) {
            bufferedBytes_ -= frames_.front().data.size();
            frames_.pop_front();
        }
    }

    // Update keyframe tracking after eviction
    updateKeyframeIndex();
}

void GOPBuffer::updateKeyframeIndex() {
    // This is called with mutex already held

    hasKeyframe_ = false;
    lastKeyframeIndex_ = 0;

    // Find the last keyframe in the buffer
    for (size_t i = frames_.size(); i > 0; --i) {
        const auto& frame = frames_[i - 1];
        if (frame.isKeyframe && frame.type == MediaType::Video) {
            lastKeyframeIndex_ = i - 1;
            hasKeyframe_ = true;
            break;
        }
    }
}

} // namespace streaming
} // namespace openrtmp
