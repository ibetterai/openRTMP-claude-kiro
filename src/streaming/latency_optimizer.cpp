// OpenRTMP - Cross-platform RTMP Server
// Latency Optimizer Implementation
//
// Requirements coverage:
// - Requirement 12.1: Glass-to-glass latency <2s on desktop
// - Requirement 12.2: Glass-to-glass latency <3s on mobile
// - Requirement 12.3: Process and forward chunks within 50ms
// - Requirement 12.4: Low-latency mode option
// - Requirement 12.5: Low-latency mode 500ms maximum buffer

#include "openrtmp/streaming/latency_optimizer.hpp"

#include <algorithm>
#include <numeric>

namespace openrtmp {
namespace streaming {

// =============================================================================
// Constructor and Destructor
// =============================================================================

LatencyOptimizer::LatencyOptimizer()
    : config_(LatencyConfig::forCurrentPlatform()) {
}

LatencyOptimizer::LatencyOptimizer(const LatencyConfig& config)
    : config_(config) {
}

LatencyOptimizer::LatencyOptimizer(LatencyOptimizer&& other) noexcept {
    std::unique_lock<std::shared_mutex> configLock(other.configMutex_);
    std::unique_lock<std::shared_mutex> chunkLock(other.chunkMutex_);
    std::unique_lock<std::mutex> procLock(other.procStatsMutex_);
    std::unique_lock<std::shared_mutex> streamLock(other.streamMutex_);
    std::unique_lock<std::mutex> callbackLock(other.callbackMutex_);

    config_ = std::move(other.config_);
    chunkTracking_ = std::move(other.chunkTracking_);
    totalChunksProcessed_ = other.totalChunksProcessed_;
    slowChunksCount_ = other.slowChunksCount_;
    totalProcessingTime_ = other.totalProcessingTime_;
    minProcessingTime_ = other.minProcessingTime_;
    maxProcessingTime_ = other.maxProcessingTime_;
    streamTracking_ = std::move(other.streamTracking_);
    onHighLatencyCallback_ = std::move(other.onHighLatencyCallback_);
    onSlowProcessingCallback_ = std::move(other.onSlowProcessingCallback_);
}

LatencyOptimizer& LatencyOptimizer::operator=(LatencyOptimizer&& other) noexcept {
    if (this != &other) {
        std::unique_lock<std::shared_mutex> configLock(configMutex_, std::defer_lock);
        std::unique_lock<std::shared_mutex> chunkLock(chunkMutex_, std::defer_lock);
        std::unique_lock<std::mutex> procLock(procStatsMutex_, std::defer_lock);
        std::unique_lock<std::shared_mutex> streamLock(streamMutex_, std::defer_lock);
        std::unique_lock<std::mutex> callbackLock(callbackMutex_, std::defer_lock);

        std::unique_lock<std::shared_mutex> otherConfigLock(other.configMutex_, std::defer_lock);
        std::unique_lock<std::shared_mutex> otherChunkLock(other.chunkMutex_, std::defer_lock);
        std::unique_lock<std::mutex> otherProcLock(other.procStatsMutex_, std::defer_lock);
        std::unique_lock<std::shared_mutex> otherStreamLock(other.streamMutex_, std::defer_lock);
        std::unique_lock<std::mutex> otherCallbackLock(other.callbackMutex_, std::defer_lock);

        std::lock(configLock, chunkLock, procLock, streamLock, callbackLock,
                  otherConfigLock, otherChunkLock, otherProcLock, otherStreamLock, otherCallbackLock);

        config_ = std::move(other.config_);
        chunkTracking_ = std::move(other.chunkTracking_);
        totalChunksProcessed_ = other.totalChunksProcessed_;
        slowChunksCount_ = other.slowChunksCount_;
        totalProcessingTime_ = other.totalProcessingTime_;
        minProcessingTime_ = other.minProcessingTime_;
        maxProcessingTime_ = other.maxProcessingTime_;
        streamTracking_ = std::move(other.streamTracking_);
        onHighLatencyCallback_ = std::move(other.onHighLatencyCallback_);
        onSlowProcessingCallback_ = std::move(other.onSlowProcessingCallback_);
    }
    return *this;
}

// =============================================================================
// Configuration
// =============================================================================

void LatencyOptimizer::setConfig(const LatencyConfig& config) {
    std::unique_lock<std::shared_mutex> lock(configMutex_);
    config_ = config;
}

LatencyConfig LatencyOptimizer::getConfig() const {
    std::shared_lock<std::shared_mutex> lock(configMutex_);
    return config_;
}

void LatencyOptimizer::enableLowLatencyMode() {
    std::unique_lock<std::shared_mutex> lock(configMutex_);
    config_.lowLatencyMode = true;
    config_.maxSubscriberBuffer = std::chrono::milliseconds(500);
}

void LatencyOptimizer::disableLowLatencyMode() {
    std::unique_lock<std::shared_mutex> lock(configMutex_);
    config_.lowLatencyMode = false;
    config_.maxSubscriberBuffer = config_.normalModeBuffer;
}

// =============================================================================
// Chunk Processing Tracking
// =============================================================================

void LatencyOptimizer::recordChunkReceived(uint32_t chunkId) {
    std::unique_lock<std::shared_mutex> lock(chunkMutex_);

    ChunkTracking tracking;
    tracking.receiveTime = std::chrono::steady_clock::now();
    chunkTracking_[chunkId] = tracking;
}

void LatencyOptimizer::recordChunkForwarded(uint32_t chunkId) {
    auto forwardTime = std::chrono::steady_clock::now();
    std::chrono::milliseconds processingTime{0};
    bool wasSlow = false;

    {
        std::unique_lock<std::shared_mutex> lock(chunkMutex_);

        auto it = chunkTracking_.find(chunkId);
        if (it == chunkTracking_.end()) {
            return;  // Chunk not tracked
        }

        it->second.forwardTime = forwardTime;
        processingTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            forwardTime - it->second.receiveTime
        );

        std::shared_lock<std::shared_mutex> configLock(configMutex_);
        wasSlow = processingTime > config_.maxProcessingTime;
        it->second.slow = wasSlow;
    }

    updateProcessingStats(processingTime, wasSlow);

    if (wasSlow) {
        invokeSlowProcessingCallback(chunkId, processingTime);
    }
}

std::optional<std::chrono::steady_clock::time_point>
LatencyOptimizer::getChunkReceiveTime(uint32_t chunkId) const {
    std::shared_lock<std::shared_mutex> lock(chunkMutex_);

    auto it = chunkTracking_.find(chunkId);
    if (it == chunkTracking_.end()) {
        return std::nullopt;
    }
    return it->second.receiveTime;
}

std::optional<std::chrono::milliseconds>
LatencyOptimizer::getChunkProcessingTime(uint32_t chunkId) const {
    std::shared_lock<std::shared_mutex> lock(chunkMutex_);

    auto it = chunkTracking_.find(chunkId);
    if (it == chunkTracking_.end() || !it->second.forwardTime) {
        return std::nullopt;
    }

    return std::chrono::duration_cast<std::chrono::milliseconds>(
        *it->second.forwardTime - it->second.receiveTime
    );
}

bool LatencyOptimizer::wasProcessingTimeSlow(uint32_t chunkId) const {
    std::shared_lock<std::shared_mutex> lock(chunkMutex_);

    auto it = chunkTracking_.find(chunkId);
    if (it == chunkTracking_.end()) {
        return false;
    }
    return it->second.slow;
}

// =============================================================================
// Stream Latency Tracking
// =============================================================================

void LatencyOptimizer::startStreamTracking(const StreamKey& key) {
    std::unique_lock<std::shared_mutex> lock(streamMutex_);

    auto data = std::make_unique<StreamTrackingData>();
    data->key = key;
    data->trackingStarted = std::chrono::steady_clock::now();
    streamTracking_[key] = std::move(data);
}

void LatencyOptimizer::stopStreamTracking(const StreamKey& key) {
    std::unique_lock<std::shared_mutex> lock(streamMutex_);
    streamTracking_.erase(key);
}

bool LatencyOptimizer::isTrackingStream(const StreamKey& key) const {
    std::shared_lock<std::shared_mutex> lock(streamMutex_);
    return streamTracking_.find(key) != streamTracking_.end();
}

std::vector<StreamKey> LatencyOptimizer::getTrackedStreams() const {
    std::shared_lock<std::shared_mutex> lock(streamMutex_);

    std::vector<StreamKey> keys;
    keys.reserve(streamTracking_.size());
    for (const auto& [key, _] : streamTracking_) {
        keys.push_back(key);
    }
    return keys;
}

void LatencyOptimizer::recordFrameLatency(
    const StreamKey& key,
    std::chrono::milliseconds latency
) {
    bool exceedsThreshold = false;
    std::chrono::milliseconds threshold{0};

    {
        std::shared_lock<std::shared_mutex> streamLock(streamMutex_);

        auto it = streamTracking_.find(key);
        if (it == streamTracking_.end()) {
            return;  // Stream not tracked
        }

        {
            std::lock_guard<std::mutex> dataLock(it->second->mutex);
            it->second->latencySamples.push_back(latency);
        }

        {
            std::shared_lock<std::shared_mutex> configLock(configMutex_);
            threshold = config_.alertThreshold;
            exceedsThreshold = latency > threshold;
        }
    }

    if (exceedsThreshold) {
        invokeHighLatencyCallback(key, latency);
    }
}

std::optional<StreamLatencyStatistics>
LatencyOptimizer::getStreamLatency(const StreamKey& key) const {
    std::shared_lock<std::shared_mutex> streamLock(streamMutex_);

    auto it = streamTracking_.find(key);
    if (it == streamTracking_.end()) {
        return std::nullopt;
    }

    StreamLatencyStatistics stats;
    stats.streamKey = key;
    stats.trackingStarted = it->second->trackingStarted;

    std::vector<std::chrono::milliseconds> samples;
    {
        std::lock_guard<std::mutex> dataLock(it->second->mutex);
        samples = it->second->latencySamples;
    }

    if (samples.empty()) {
        stats.sampleCount = 0;
        return stats;
    }

    stats.sampleCount = samples.size();

    // Calculate average
    auto total = std::accumulate(samples.begin(), samples.end(),
                                  std::chrono::milliseconds(0));
    stats.averageLatency = std::chrono::milliseconds(
        total.count() / static_cast<int64_t>(samples.size())
    );

    // Find min and max
    auto [minIt, maxIt] = std::minmax_element(samples.begin(), samples.end());
    stats.minLatency = *minIt;
    stats.maxLatency = *maxIt;

    // Sort for percentile calculation
    std::sort(samples.begin(), samples.end());

    stats.p50Latency = calculatePercentile(samples, 0.50);
    stats.p95Latency = calculatePercentile(samples, 0.95);
    stats.p99Latency = calculatePercentile(samples, 0.99);

    // Check if within target
    {
        std::shared_lock<std::shared_mutex> configLock(configMutex_);
        stats.withinTarget = stats.averageLatency <= config_.targetGlassToGlassLatency;
    }

    return stats;
}

bool LatencyOptimizer::isWithinLatencyTarget(const StreamKey& key) const {
    auto stats = getStreamLatency(key);
    if (!stats || stats->sampleCount == 0) {
        return true;  // No data, assume within target
    }
    return stats->withinTarget;
}

// =============================================================================
// Statistics
// =============================================================================

ProcessingStatistics LatencyOptimizer::getProcessingStatistics() const {
    std::lock_guard<std::mutex> lock(procStatsMutex_);

    ProcessingStatistics stats;
    stats.totalChunksProcessed = totalChunksProcessed_;
    stats.slowChunksCount = slowChunksCount_;

    if (totalChunksProcessed_ > 0) {
        stats.averageProcessingTime = std::chrono::milliseconds(
            totalProcessingTime_.count() / static_cast<int64_t>(totalChunksProcessed_)
        );
        stats.minProcessingTime = minProcessingTime_;
        stats.maxProcessingTime = maxProcessingTime_;
        stats.onTimePercentage = 100.0 *
            static_cast<double>(totalChunksProcessed_ - slowChunksCount_) /
            static_cast<double>(totalChunksProcessed_);
    }

    return stats;
}

std::chrono::milliseconds LatencyOptimizer::getRecommendedBufferSize() const {
    std::shared_lock<std::shared_mutex> lock(configMutex_);

    if (config_.lowLatencyMode) {
        return config_.maxSubscriberBuffer;  // 500ms in low-latency mode
    }
    return config_.normalModeBuffer;  // 5000ms in normal mode
}

// =============================================================================
// Callbacks
// =============================================================================

void LatencyOptimizer::setOnHighLatencyCallback(OnHighLatencyCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    onHighLatencyCallback_ = std::move(callback);
}

void LatencyOptimizer::setOnSlowProcessingCallback(OnSlowProcessingCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    onSlowProcessingCallback_ = std::move(callback);
}

// =============================================================================
// Lifecycle
// =============================================================================

void LatencyOptimizer::reset() {
    // Clear chunk tracking
    {
        std::unique_lock<std::shared_mutex> lock(chunkMutex_);
        chunkTracking_.clear();
    }

    // Reset processing statistics
    {
        std::lock_guard<std::mutex> lock(procStatsMutex_);
        totalChunksProcessed_ = 0;
        slowChunksCount_ = 0;
        totalProcessingTime_ = std::chrono::milliseconds(0);
        minProcessingTime_ = std::chrono::milliseconds::max();
        maxProcessingTime_ = std::chrono::milliseconds(0);
    }

    // Clear stream tracking
    {
        std::unique_lock<std::shared_mutex> lock(streamMutex_);
        streamTracking_.clear();
    }
}

// =============================================================================
// Private Helpers
// =============================================================================

std::chrono::milliseconds LatencyOptimizer::calculatePercentile(
    const std::vector<std::chrono::milliseconds>& sortedSamples,
    double percentile
) const {
    if (sortedSamples.empty()) {
        return std::chrono::milliseconds(0);
    }

    size_t index = static_cast<size_t>(
        percentile * static_cast<double>(sortedSamples.size() - 1)
    );
    return sortedSamples[index];
}

void LatencyOptimizer::updateProcessingStats(
    std::chrono::milliseconds processingTime,
    bool wasSlow
) {
    std::lock_guard<std::mutex> lock(procStatsMutex_);

    totalChunksProcessed_++;
    totalProcessingTime_ += processingTime;

    if (wasSlow) {
        slowChunksCount_++;
    }

    if (processingTime < minProcessingTime_) {
        minProcessingTime_ = processingTime;
    }
    if (processingTime > maxProcessingTime_) {
        maxProcessingTime_ = processingTime;
    }
}

void LatencyOptimizer::invokeHighLatencyCallback(
    const StreamKey& key,
    std::chrono::milliseconds latency
) {
    OnHighLatencyCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = onHighLatencyCallback_;
    }

    if (callback) {
        callback(key, latency);
    }
}

void LatencyOptimizer::invokeSlowProcessingCallback(
    uint32_t chunkId,
    std::chrono::milliseconds time
) {
    OnSlowProcessingCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = onSlowProcessingCallback_;
    }

    if (callback) {
        callback(chunkId, time);
    }
}

} // namespace streaming
} // namespace openrtmp
