// OpenRTMP - Cross-platform RTMP Server
// Publisher Lifecycle Implementation

#include "openrtmp/streaming/publisher_lifecycle.hpp"

#include <algorithm>
#include <cstring>

namespace openrtmp {
namespace streaming {

// =============================================================================
// Constructor / Destructor
// =============================================================================

PublisherLifecycle::PublisherLifecycle()
    : active_(false)
    , disconnected_(false)
    , disconnectNotified_(false)
    , hasFirstVideoTimestamp_(false)
    , hasFirstAudioTimestamp_(false)
    , bytesInWindow_(0)
    , unavailabilityTimeoutMs_(lifecycle::UNAVAILABLE_TIMEOUT_MS)
{
}

PublisherLifecycle::~PublisherLifecycle() {
    // Clear callbacks to avoid calling into destroyed objects
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        timestampGapCallback_ = nullptr;
        streamUnavailableCallback_ = nullptr;
        disconnectCallback_ = nullptr;
        subscriberNotificationCallback_ = nullptr;
    }

    active_ = false;
}

PublisherLifecycle::PublisherLifecycle(PublisherLifecycle&& other) noexcept
    : active_(other.active_.load())
    , disconnected_(other.disconnected_.load())
    , disconnectNotified_(other.disconnectNotified_.load())
    , stats_(std::move(other.stats_))
    , hasFirstVideoTimestamp_(other.hasFirstVideoTimestamp_)
    , hasFirstAudioTimestamp_(other.hasFirstAudioTimestamp_)
    , subscribers_(std::move(other.subscribers_))
    , startTime_(other.startTime_)
    , lastBitrateCalcTime_(other.lastBitrateCalcTime_)
    , bytesInWindow_(other.bytesInWindow_)
    , timestampGapCallback_(std::move(other.timestampGapCallback_))
    , streamUnavailableCallback_(std::move(other.streamUnavailableCallback_))
    , disconnectCallback_(std::move(other.disconnectCallback_))
    , subscriberNotificationCallback_(std::move(other.subscriberNotificationCallback_))
    , unavailabilityTimeoutMs_(other.unavailabilityTimeoutMs_)
{
    other.active_ = false;
}

PublisherLifecycle& PublisherLifecycle::operator=(PublisherLifecycle&& other) noexcept {
    if (this != &other) {
        stop();

        std::lock_guard<std::mutex> lock(mutex_);
        std::lock_guard<std::mutex> otherLock(other.mutex_);

        active_ = other.active_.load();
        disconnected_ = other.disconnected_.load();
        disconnectNotified_ = other.disconnectNotified_.load();
        stats_ = std::move(other.stats_);
        hasFirstVideoTimestamp_ = other.hasFirstVideoTimestamp_;
        hasFirstAudioTimestamp_ = other.hasFirstAudioTimestamp_;
        subscribers_ = std::move(other.subscribers_);
        startTime_ = other.startTime_;
        lastBitrateCalcTime_ = other.lastBitrateCalcTime_;
        bytesInWindow_ = other.bytesInWindow_;

        {
            std::lock_guard<std::mutex> callbackLock(callbackMutex_);
            timestampGapCallback_ = std::move(other.timestampGapCallback_);
            streamUnavailableCallback_ = std::move(other.streamUnavailableCallback_);
            disconnectCallback_ = std::move(other.disconnectCallback_);
            subscriberNotificationCallback_ = std::move(other.subscriberNotificationCallback_);
        }

        other.active_ = false;
    }
    return *this;
}

// =============================================================================
// Lifecycle Control
// =============================================================================

void PublisherLifecycle::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (active_) {
        return;  // Already active
    }

    resetState();
    active_ = true;
    startTime_ = std::chrono::steady_clock::now();
    lastBitrateCalcTime_ = startTime_;
}

void PublisherLifecycle::stop() {
    bool wasActive = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wasActive = active_;
        active_ = false;
    }

    if (!wasActive) {
        return;  // Already stopped
    }

    // Notify all subscribers of stream end
    notifyAllSubscribers(StreamNotification::StreamEnded);

    // Clear subscribers
    {
        std::lock_guard<std::mutex> lock(mutex_);
        subscribers_.clear();
    }
}

bool PublisherLifecycle::isActive() const {
    return active_.load();
}

// =============================================================================
// Media Processing
// =============================================================================

void PublisherLifecycle::onMediaReceived(const MediaMessage& msg) {
    if (!active_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Update statistics
    stats_.totalBytesReceived += msg.payload.size();

    switch (msg.type) {
        case MediaType::Video:
            stats_.videoFrameCount++;
            if (msg.isKeyframe) {
                stats_.keyframeCount++;
            }
            checkTimestampGap(MediaType::Video, msg.timestamp);
            stats_.lastVideoTimestamp = msg.timestamp;
            break;

        case MediaType::Audio:
            stats_.audioFrameCount++;
            checkTimestampGap(MediaType::Audio, msg.timestamp);
            stats_.lastAudioTimestamp = msg.timestamp;
            break;

        case MediaType::Data:
            // Data messages don't contribute to frame counts
            break;
    }

    // Update bitrate calculation
    updateBitrate(msg.payload.size());

    // Update duration
    auto now = std::chrono::steady_clock::now();
    stats_.durationMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime_).count()
    );
}

// =============================================================================
// Publisher Disconnect Handling
// =============================================================================

void PublisherLifecycle::onPublisherDisconnected(DisconnectReason reason) {
    bool alreadyNotified = disconnectNotified_.exchange(true);
    if (alreadyNotified) {
        return;  // Already handled
    }

    disconnected_ = true;

    // Invoke disconnect callback
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (disconnectCallback_) {
            disconnectCallback_(reason);
        }
    }

    if (reason == DisconnectReason::Graceful) {
        // For graceful disconnect, immediately notify subscribers of stream end
        notifyAllSubscribers(StreamNotification::StreamEnded);
    } else {
        // For unexpected disconnect, mark stream unavailable within configured timeout
        // NOTE: In a production system, this would use the network layer's async timer
        // to delay the notification by up to 5 seconds. For this component, we invoke
        // the unavailability notification synchronously. The actual timer-based delay
        // should be implemented at the session/connection layer which has access to
        // the event loop.

        // Invoke stream unavailable callback
        StreamUnavailableCallback unavailableCallback;
        {
            std::lock_guard<std::mutex> callbackLock(callbackMutex_);
            unavailableCallback = streamUnavailableCallback_;
        }

        if (unavailableCallback) {
            unavailableCallback();
        }

        // Notify all subscribers of stream unavailability
        notifyAllSubscribers(StreamNotification::StreamUnavailable);
    }
}

// =============================================================================
// Subscriber Management
// =============================================================================

void PublisherLifecycle::addSubscriber(SubscriberId subscriberId) {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.insert(subscriberId);
}

void PublisherLifecycle::removeSubscriber(SubscriberId subscriberId) {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.erase(subscriberId);
}

size_t PublisherLifecycle::getSubscriberCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscribers_.size();
}

// =============================================================================
// Statistics
// =============================================================================

StreamStatistics PublisherLifecycle::getStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

// =============================================================================
// Callback Setters
// =============================================================================

void PublisherLifecycle::setTimestampGapCallback(TimestampGapCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    timestampGapCallback_ = std::move(callback);
}

void PublisherLifecycle::setStreamUnavailableCallback(StreamUnavailableCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    streamUnavailableCallback_ = std::move(callback);
}

void PublisherLifecycle::setDisconnectCallback(DisconnectCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    disconnectCallback_ = std::move(callback);
}

void PublisherLifecycle::setSubscriberNotificationCallback(SubscriberNotificationCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    subscriberNotificationCallback_ = std::move(callback);
}

void PublisherLifecycle::setUnavailabilityTimeout(uint32_t timeoutMs) {
    unavailabilityTimeoutMs_ = timeoutMs;
}

// =============================================================================
// Internal Methods
// =============================================================================

void PublisherLifecycle::checkTimestampGap(MediaType type, uint32_t timestamp) {
    // Must be called with mutex held

    uint32_t previousTimestamp = 0;
    bool hasFirst = false;

    if (type == MediaType::Video) {
        previousTimestamp = stats_.lastVideoTimestamp;
        hasFirst = hasFirstVideoTimestamp_;
        hasFirstVideoTimestamp_ = true;
    } else if (type == MediaType::Audio) {
        previousTimestamp = stats_.lastAudioTimestamp;
        hasFirst = hasFirstAudioTimestamp_;
        hasFirstAudioTimestamp_ = true;
    }

    if (!hasFirst) {
        // First frame of this type, no gap check needed
        return;
    }

    // Check for timestamp wraparound (don't report as gap)
    if (isTimestampWraparound(previousTimestamp, timestamp)) {
        return;
    }

    // Calculate gap
    uint32_t gap = 0;
    if (timestamp > previousTimestamp) {
        gap = timestamp - previousTimestamp;
    } else {
        // Timestamp went backwards (not wraparound), might be reordering
        return;
    }

    // Check if gap exceeds threshold (> 1 second = 1000ms)
    if (gap > lifecycle::TIMESTAMP_GAP_THRESHOLD_MS) {
        stats_.timestampGapCount++;

        TimestampGapInfo gapInfo;
        gapInfo.mediaType = type;
        gapInfo.previousTimestamp = previousTimestamp;
        gapInfo.currentTimestamp = timestamp;
        gapInfo.gapMs = gap;

        // Invoke callback outside lock to avoid deadlock
        TimestampGapCallback callback;
        {
            std::lock_guard<std::mutex> callbackLock(callbackMutex_);
            callback = timestampGapCallback_;
        }

        if (callback) {
            // Release main mutex before calling callback
            mutex_.unlock();
            callback(gapInfo);
            mutex_.lock();
        }
    }
}

bool PublisherLifecycle::isTimestampWraparound(uint32_t previous, uint32_t current) const {
    // RTMP timestamps are 32-bit and wrap around at 2^32
    // Consider it a wraparound if previous is near max and current is near zero
    constexpr uint32_t WRAPAROUND_THRESHOLD = 0x80000000;  // Half of max uint32

    return (previous > WRAPAROUND_THRESHOLD) && (current < (0xFFFFFFFF - WRAPAROUND_THRESHOLD));
}

void PublisherLifecycle::updateBitrate(size_t bytes) {
    // Must be called with mutex held

    auto now = std::chrono::steady_clock::now();
    bytesInWindow_ += bytes;

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastBitrateCalcTime_).count();

    if (elapsed >= lifecycle::BITRATE_WINDOW_MS) {
        // Calculate bitrate: (bytes * 8) / (time in seconds)
        // = (bytes * 8 * 1000) / (time in milliseconds)
        if (elapsed > 0) {
            stats_.currentBitrateBps = (bytesInWindow_ * 8 * 1000) / static_cast<uint64_t>(elapsed);
        }

        // Reset window
        bytesInWindow_ = 0;
        lastBitrateCalcTime_ = now;
    }
}

void PublisherLifecycle::notifyAllSubscribers(StreamNotification notification) {
    std::set<SubscriberId> subscribersCopy;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        subscribersCopy = subscribers_;
    }

    SubscriberNotificationCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = subscriberNotificationCallback_;
    }

    if (callback) {
        for (SubscriberId subId : subscribersCopy) {
            callback(subId, notification);
        }
    }
}

void PublisherLifecycle::resetState() {
    // Must be called with mutex held or during construction

    stats_ = StreamStatistics();
    hasFirstVideoTimestamp_ = false;
    hasFirstAudioTimestamp_ = false;
    disconnected_ = false;
    disconnectNotified_ = false;
    bytesInWindow_ = 0;
}

} // namespace streaming
} // namespace openrtmp
