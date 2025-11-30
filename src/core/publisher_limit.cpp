// OpenRTMP - Cross-platform RTMP Server
// Publisher Limit Implementation
//
// Thread-safe implementation of publisher limit management with:
// - Platform-aware defaults (10 desktop, 3 mobile per Requirements 14.3, 14.4)
// - Acquire/release pattern for publisher slots
// - Statistics tracking for metrics integration
// - Callback notifications for limit events
//
// Requirements coverage:
// - Requirement 14.3: Desktop platforms support at least 10 simultaneous publishing streams
// - Requirement 14.4: Mobile platforms support at least 3 simultaneous publishing streams

#include "openrtmp/core/publisher_limit.hpp"

#include <algorithm>

namespace openrtmp {
namespace core {

// =============================================================================
// Constructor / Destructor
// =============================================================================

PublisherLimit::PublisherLimit(const PublisherLimitConfig& config)
    : config_(config)
{
    // Ensure minimum of 1 publisher
    if (config_.maxPublishers == 0) {
        config_.maxPublishers = 1;
    }
}

PublisherLimit::~PublisherLimit() = default;

PublisherLimit::PublisherLimit(PublisherLimit&& other) noexcept
    : config_(other.config_)
    , activePublishers_(std::move(other.activePublishers_))
    , peakPublishers_(other.peakPublishers_)
    , totalAcquires_(other.totalAcquires_)
    , totalReleases_(other.totalReleases_)
    , rejectedAcquires_(other.rejectedAcquires_)
    , highWatermarkTriggered_(other.highWatermarkTriggered_)
    , callback_(std::move(other.callback_))
{
}

PublisherLimit& PublisherLimit::operator=(PublisherLimit&& other) noexcept {
    if (this != &other) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        config_ = other.config_;
        activePublishers_ = std::move(other.activePublishers_);
        peakPublishers_ = other.peakPublishers_;
        totalAcquires_ = other.totalAcquires_;
        totalReleases_ = other.totalReleases_;
        rejectedAcquires_ = other.rejectedAcquires_;
        highWatermarkTriggered_ = other.highWatermarkTriggered_;

        std::lock_guard<std::mutex> callbackLock(callbackMutex_);
        callback_ = std::move(other.callback_);
    }
    return *this;
}

// =============================================================================
// Publisher Slot Management
// =============================================================================

Result<void, Error> PublisherLimit::acquirePublisherSlot(PublisherId publisherId) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Check if publisher already has a slot
    if (activePublishers_.find(publisherId) != activePublishers_.end()) {
        return Result<void, Error>::error(Error{
            ErrorCode::AlreadyExists,
            "Publisher already has an active slot: " + std::to_string(publisherId)
        });
    }

    // Check if limit reached
    if (activePublishers_.size() >= config_.maxPublishers) {
        ++rejectedAcquires_;

        // Emit limit reached event
        lock.unlock();
        emitEvent(PublisherLimitEventType::LimitReached, publisherId);
        emitEvent(PublisherLimitEventType::PublisherRejected, publisherId);

        return Result<void, Error>::error(Error{
            ErrorCode::StreamLimitReached,
            "Publisher limit reached (" + std::to_string(config_.maxPublishers) + ")"
        });
    }

    // Track previous count for watermark checking
    size_t previousCount = activePublishers_.size();

    // Acquire the slot
    activePublishers_.insert(publisherId);
    ++totalAcquires_;

    // Update peak
    if (activePublishers_.size() > peakPublishers_) {
        peakPublishers_ = activePublishers_.size();
    }

    size_t currentCount = activePublishers_.size();

    lock.unlock();

    // Check and emit watermark events
    checkWatermarks(previousCount, currentCount);

    // Emit acquire event
    emitEvent(PublisherLimitEventType::PublisherAcquired, publisherId);

    return Result<void, Error>::success();
}

void PublisherLimit::releasePublisherSlot(PublisherId publisherId) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Check if publisher has a slot
    auto it = activePublishers_.find(publisherId);
    if (it == activePublishers_.end()) {
        // Publisher not found - ignore silently
        return;
    }

    // Track previous count for watermark checking
    size_t previousCount = activePublishers_.size();

    // Release the slot
    activePublishers_.erase(it);
    ++totalReleases_;

    size_t currentCount = activePublishers_.size();

    lock.unlock();

    // Check and emit watermark events
    checkWatermarks(previousCount, currentCount);

    // Emit release event
    emitEvent(PublisherLimitEventType::PublisherReleased, publisherId);
}

// =============================================================================
// Query Methods
// =============================================================================

bool PublisherLimit::hasPublisher(PublisherId publisherId) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return activePublishers_.find(publisherId) != activePublishers_.end();
}

bool PublisherLimit::canAcquire() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return activePublishers_.size() < config_.maxPublishers;
}

size_t PublisherLimit::getActivePublisherCount() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return activePublishers_.size();
}

size_t PublisherLimit::getRemainingSlots() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return config_.maxPublishers - activePublishers_.size();
}

std::set<PublisherId> PublisherLimit::getActivePublisherIds() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return activePublishers_;
}

// =============================================================================
// Statistics
// =============================================================================

PublisherLimitStatistics PublisherLimit::getStatistics() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    PublisherLimitStatistics stats;
    stats.activePublishers = activePublishers_.size();
    stats.peakPublishers = peakPublishers_;
    stats.totalAcquires = totalAcquires_;
    stats.totalReleases = totalReleases_;
    stats.rejectedAcquires = rejectedAcquires_;
    stats.maxPublishers = config_.maxPublishers;

    return stats;
}

void PublisherLimit::resetStatistics() {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    peakPublishers_ = activePublishers_.size();  // Reset peak to current
    totalAcquires_ = 0;
    totalReleases_ = 0;
    rejectedAcquires_ = 0;
}

// =============================================================================
// Configuration
// =============================================================================

PublisherLimitConfig PublisherLimit::getConfig() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return config_;
}

// =============================================================================
// Callbacks
// =============================================================================

void PublisherLimit::setPublisherLimitCallback(PublisherLimitCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callback_ = std::move(callback);
}

// =============================================================================
// Lifecycle
// =============================================================================

void PublisherLimit::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    activePublishers_.clear();
    highWatermarkTriggered_ = false;
}

// =============================================================================
// Private Methods
// =============================================================================

void PublisherLimit::emitEvent(PublisherLimitEventType type, PublisherId publisherId) {
    std::lock_guard<std::mutex> lock(callbackMutex_);

    if (!callback_) {
        return;
    }

    PublisherLimitEvent event;
    event.type = type;
    event.publisherId = publisherId;

    // Get current state
    {
        std::shared_lock<std::shared_mutex> stateLock(mutex_);
        event.currentCount = activePublishers_.size();
        event.maxCount = config_.maxPublishers;
    }

    event.timestamp = std::chrono::steady_clock::now();

    callback_(event);
}

void PublisherLimit::checkWatermarks(size_t previousCount, size_t currentCount) {
    // Calculate thresholds
    size_t highThreshold = (config_.maxPublishers * config_.highWatermarkPercent) / 100;
    size_t lowThreshold = (config_.maxPublishers * config_.lowWatermarkPercent) / 100;

    // Check high watermark (crossing up)
    if (!highWatermarkTriggered_ && currentCount >= highThreshold && previousCount < highThreshold) {
        highWatermarkTriggered_ = true;
        emitEvent(PublisherLimitEventType::HighWatermark);
    }

    // Check low watermark (crossing down)
    if (highWatermarkTriggered_ && currentCount <= lowThreshold && previousCount > lowThreshold) {
        highWatermarkTriggered_ = false;
        emitEvent(PublisherLimitEventType::LowWatermark);
    }
}

} // namespace core
} // namespace openrtmp
