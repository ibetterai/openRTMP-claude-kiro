// OpenRTMP - Cross-platform RTMP Server
// Throughput Optimizer Implementation
//
// Requirements coverage:
// - Requirement 13.1: 50 Mbps ingestion on desktop
// - Requirement 13.2: 20 Mbps ingestion on mobile
// - Requirement 13.3: 500 Mbps aggregate distribution on desktop
// - Requirement 13.4: 100 Mbps aggregate distribution on mobile
// - Requirement 13.6: Log warnings for exceeding bitrate limits

#include "openrtmp/streaming/throughput_optimizer.hpp"

#include <algorithm>
#include <numeric>

namespace openrtmp {
namespace streaming {

// =============================================================================
// Constructor and Destructor
// =============================================================================

ThroughputOptimizer::ThroughputOptimizer()
    : config_(ThroughputConfig::forCurrentPlatform()) {
}

ThroughputOptimizer::ThroughputOptimizer(const ThroughputConfig& config)
    : config_(config) {
}

ThroughputOptimizer::ThroughputOptimizer(ThroughputOptimizer&& other) noexcept {
    std::unique_lock<std::shared_mutex> configLock(other.configMutex_);
    std::unique_lock<std::shared_mutex> streamLock(other.streamMutex_);
    std::unique_lock<std::mutex> aggregateLock(other.aggregateMutex_);
    std::unique_lock<std::mutex> callbackLock(other.callbackMutex_);

    config_ = std::move(other.config_);
    streamTracking_ = std::move(other.streamTracking_);
    aggregateIngestionSamples_ = std::move(other.aggregateIngestionSamples_);
    aggregateDistributionSamples_ = std::move(other.aggregateDistributionSamples_);
    totalAggregateIngestionBytes_ = other.totalAggregateIngestionBytes_;
    totalAggregateDistributionBytes_ = other.totalAggregateDistributionBytes_;
    lastAggregateWarning_ = other.lastAggregateWarning_;
    onWarningCallback_ = std::move(other.onWarningCallback_);
}

ThroughputOptimizer& ThroughputOptimizer::operator=(ThroughputOptimizer&& other) noexcept {
    if (this != &other) {
        std::unique_lock<std::shared_mutex> configLock(configMutex_, std::defer_lock);
        std::unique_lock<std::shared_mutex> streamLock(streamMutex_, std::defer_lock);
        std::unique_lock<std::mutex> aggregateLock(aggregateMutex_, std::defer_lock);
        std::unique_lock<std::mutex> callbackLock(callbackMutex_, std::defer_lock);

        std::unique_lock<std::shared_mutex> otherConfigLock(other.configMutex_, std::defer_lock);
        std::unique_lock<std::shared_mutex> otherStreamLock(other.streamMutex_, std::defer_lock);
        std::unique_lock<std::mutex> otherAggregateLock(other.aggregateMutex_, std::defer_lock);
        std::unique_lock<std::mutex> otherCallbackLock(other.callbackMutex_, std::defer_lock);

        std::lock(configLock, streamLock, aggregateLock, callbackLock,
                  otherConfigLock, otherStreamLock, otherAggregateLock, otherCallbackLock);

        config_ = std::move(other.config_);
        streamTracking_ = std::move(other.streamTracking_);
        aggregateIngestionSamples_ = std::move(other.aggregateIngestionSamples_);
        aggregateDistributionSamples_ = std::move(other.aggregateDistributionSamples_);
        totalAggregateIngestionBytes_ = other.totalAggregateIngestionBytes_;
        totalAggregateDistributionBytes_ = other.totalAggregateDistributionBytes_;
        lastAggregateWarning_ = other.lastAggregateWarning_;
        onWarningCallback_ = std::move(other.onWarningCallback_);
    }
    return *this;
}

// =============================================================================
// Configuration
// =============================================================================

void ThroughputOptimizer::setConfig(const ThroughputConfig& config) {
    std::unique_lock<std::shared_mutex> lock(configMutex_);
    config_ = config;
}

ThroughputConfig ThroughputOptimizer::getConfig() const {
    std::shared_lock<std::shared_mutex> lock(configMutex_);
    return config_;
}

// =============================================================================
// Stream Tracking
// =============================================================================

void ThroughputOptimizer::startStreamTracking(const StreamKey& key) {
    std::unique_lock<std::shared_mutex> lock(streamMutex_);

    auto data = std::make_unique<StreamTrackingData>();
    data->key = key;
    data->trackingStarted = std::chrono::steady_clock::now();
    streamTracking_[key] = std::move(data);
}

void ThroughputOptimizer::stopStreamTracking(const StreamKey& key) {
    std::unique_lock<std::shared_mutex> lock(streamMutex_);
    streamTracking_.erase(key);
}

bool ThroughputOptimizer::isTrackingStream(const StreamKey& key) const {
    std::shared_lock<std::shared_mutex> lock(streamMutex_);
    return streamTracking_.find(key) != streamTracking_.end();
}

std::vector<StreamKey> ThroughputOptimizer::getTrackedStreams() const {
    std::shared_lock<std::shared_mutex> lock(streamMutex_);

    std::vector<StreamKey> keys;
    keys.reserve(streamTracking_.size());
    for (const auto& [key, _] : streamTracking_) {
        keys.push_back(key);
    }
    return keys;
}

// =============================================================================
// Data Recording
// =============================================================================

void ThroughputOptimizer::recordIngestionData(const StreamKey& key, size_t bytes) {
    auto now = std::chrono::steady_clock::now();
    ThroughputConfig currentConfig;
    double currentBitrate = 0.0;

    {
        std::shared_lock<std::shared_mutex> configLock(configMutex_);
        currentConfig = config_;
    }

    // Record for stream
    {
        std::shared_lock<std::shared_mutex> streamLock(streamMutex_);

        auto it = streamTracking_.find(key);
        if (it == streamTracking_.end()) {
            return;  // Stream not tracked
        }

        {
            std::lock_guard<std::mutex> dataLock(it->second->mutex);

            // Add sample
            DataSample sample{now, bytes};
            it->second->ingestionSamples.push_back(sample);
            it->second->totalIngestionBytes += bytes;

            // Clean old samples
            cleanOldSamples(it->second->ingestionSamples, currentConfig.smoothingWindowMs);

            // Calculate current bitrate
            currentBitrate = calculateCurrentBitrate(
                it->second->ingestionSamples, currentConfig.smoothingWindowMs);

            // Update peak
            if (currentBitrate > it->second->peakIngestionBitrateMbps) {
                it->second->peakIngestionBitrateMbps = currentBitrate;
            }

            // Check warning
            if (currentConfig.warningEnabled &&
                currentBitrate > currentConfig.maxIngestionBitrateMbps) {
                checkAndWarn(key, currentBitrate, currentConfig.maxIngestionBitrateMbps,
                            ThroughputWarningType::IngestionOverLimit,
                            it->second->lastIngestionWarning);
            }
        }
    }

    // Record for aggregate
    {
        std::lock_guard<std::mutex> aggregateLock(aggregateMutex_);

        DataSample sample{now, bytes};
        aggregateIngestionSamples_.push_back(sample);
        totalAggregateIngestionBytes_ += bytes;

        cleanOldSamples(aggregateIngestionSamples_, currentConfig.smoothingWindowMs);
    }
}

void ThroughputOptimizer::recordDistributionData(const StreamKey& key, size_t bytes) {
    auto now = std::chrono::steady_clock::now();
    ThroughputConfig currentConfig;
    double currentBitrate = 0.0;

    {
        std::shared_lock<std::shared_mutex> configLock(configMutex_);
        currentConfig = config_;
    }

    // Record for stream
    {
        std::shared_lock<std::shared_mutex> streamLock(streamMutex_);

        auto it = streamTracking_.find(key);
        if (it == streamTracking_.end()) {
            return;  // Stream not tracked
        }

        {
            std::lock_guard<std::mutex> dataLock(it->second->mutex);

            // Add sample
            DataSample sample{now, bytes};
            it->second->distributionSamples.push_back(sample);
            it->second->totalDistributionBytes += bytes;

            // Clean old samples
            cleanOldSamples(it->second->distributionSamples, currentConfig.smoothingWindowMs);

            // Calculate current bitrate
            currentBitrate = calculateCurrentBitrate(
                it->second->distributionSamples, currentConfig.smoothingWindowMs);

            // Update peak
            if (currentBitrate > it->second->peakDistributionBitrateMbps) {
                it->second->peakDistributionBitrateMbps = currentBitrate;
            }

            // Check warning for per-stream distribution (only if per-stream limit is set)
            if (currentConfig.warningEnabled &&
                currentConfig.maxPerStreamDistributionBitrateMbps > 0.0 &&
                currentBitrate > currentConfig.maxPerStreamDistributionBitrateMbps) {
                checkAndWarn(key, currentBitrate, currentConfig.maxPerStreamDistributionBitrateMbps,
                            ThroughputWarningType::DistributionOverLimit,
                            it->second->lastDistributionWarning);
            }
        }
    }

    // Record for aggregate
    {
        std::lock_guard<std::mutex> aggregateLock(aggregateMutex_);

        DataSample sample{now, bytes};
        aggregateDistributionSamples_.push_back(sample);
        totalAggregateDistributionBytes_ += bytes;

        cleanOldSamples(aggregateDistributionSamples_, currentConfig.smoothingWindowMs);

        // Check aggregate distribution limit
        double aggregateBitrate = calculateCurrentBitrate(
            aggregateDistributionSamples_, currentConfig.smoothingWindowMs);

        if (currentConfig.warningEnabled &&
            aggregateBitrate > currentConfig.maxDistributionBitrateMbps) {
            checkAndWarn(StreamKey{}, aggregateBitrate, currentConfig.maxDistributionBitrateMbps,
                        ThroughputWarningType::AggregateDistributionOverLimit,
                        lastAggregateWarning_);
        }
    }
}

// =============================================================================
// Statistics
// =============================================================================

std::optional<StreamThroughputStatistics>
ThroughputOptimizer::getStreamStatistics(const StreamKey& key) const {
    ThroughputConfig currentConfig;
    {
        std::shared_lock<std::shared_mutex> configLock(configMutex_);
        currentConfig = config_;
    }

    std::shared_lock<std::shared_mutex> streamLock(streamMutex_);

    auto it = streamTracking_.find(key);
    if (it == streamTracking_.end()) {
        return std::nullopt;
    }

    StreamThroughputStatistics stats;
    stats.streamKey = key;
    stats.trackingStarted = it->second->trackingStarted;

    auto now = std::chrono::steady_clock::now();
    stats.trackingDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - it->second->trackingStarted);

    {
        std::lock_guard<std::mutex> dataLock(it->second->mutex);

        stats.totalIngestionBytes = it->second->totalIngestionBytes;
        stats.totalDistributionBytes = it->second->totalDistributionBytes;

        stats.currentIngestionBitrateMbps = calculateCurrentBitrate(
            it->second->ingestionSamples, currentConfig.smoothingWindowMs);
        stats.currentDistributionBitrateMbps = calculateCurrentBitrate(
            it->second->distributionSamples, currentConfig.smoothingWindowMs);

        stats.averageIngestionBitrateMbps = calculateAverageBitrate(
            it->second->totalIngestionBytes, it->second->trackingStarted);
        stats.averageDistributionBitrateMbps = calculateAverageBitrate(
            it->second->totalDistributionBytes, it->second->trackingStarted);

        stats.peakIngestionBitrateMbps = it->second->peakIngestionBitrateMbps;
        stats.peakDistributionBitrateMbps = it->second->peakDistributionBitrateMbps;

        stats.ingestionOverLimit =
            stats.currentIngestionBitrateMbps > currentConfig.maxIngestionBitrateMbps;
        // Per-stream distribution is only over limit if per-stream limit is set
        stats.distributionOverLimit =
            currentConfig.maxPerStreamDistributionBitrateMbps > 0.0 &&
            stats.currentDistributionBitrateMbps > currentConfig.maxPerStreamDistributionBitrateMbps;
    }

    return stats;
}

AggregateThroughputStatistics ThroughputOptimizer::getAggregateThroughput() const {
    ThroughputConfig currentConfig;
    {
        std::shared_lock<std::shared_mutex> configLock(configMutex_);
        currentConfig = config_;
    }

    AggregateThroughputStatistics stats;

    // Get stream count
    {
        std::shared_lock<std::shared_mutex> streamLock(streamMutex_);
        stats.activeStreamCount = streamTracking_.size();
    }

    // Get aggregate data
    {
        std::lock_guard<std::mutex> aggregateLock(aggregateMutex_);

        stats.totalIngestionBytes = totalAggregateIngestionBytes_;
        stats.totalDistributionBytes = totalAggregateDistributionBytes_;

        stats.currentIngestionBitrateMbps = calculateCurrentBitrate(
            aggregateIngestionSamples_, currentConfig.smoothingWindowMs);
        stats.currentDistributionBitrateMbps = calculateCurrentBitrate(
            aggregateDistributionSamples_, currentConfig.smoothingWindowMs);

        stats.ingestionOverLimit =
            stats.currentIngestionBitrateMbps > currentConfig.maxIngestionBitrateMbps;
        stats.distributionOverLimit =
            stats.currentDistributionBitrateMbps > currentConfig.maxDistributionBitrateMbps;
    }

    return stats;
}

// =============================================================================
// Limit Checks
// =============================================================================

bool ThroughputOptimizer::isIngestionOverLimit(const StreamKey& key) const {
    auto stats = getStreamStatistics(key);
    if (!stats) {
        return false;
    }
    return stats->ingestionOverLimit;
}

bool ThroughputOptimizer::isDistributionOverLimit(const StreamKey& key) const {
    auto stats = getStreamStatistics(key);
    if (!stats) {
        return false;
    }
    return stats->distributionOverLimit;
}

bool ThroughputOptimizer::isAggregateDistributionOverLimit() const {
    auto stats = getAggregateThroughput();
    return stats.distributionOverLimit;
}

// =============================================================================
// Callbacks
// =============================================================================

void ThroughputOptimizer::setOnThroughputWarningCallback(
    OnThroughputWarningCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    onWarningCallback_ = std::move(callback);
}

// =============================================================================
// Lifecycle
// =============================================================================

void ThroughputOptimizer::reset() {
    // Clear stream tracking
    {
        std::unique_lock<std::shared_mutex> lock(streamMutex_);
        streamTracking_.clear();
    }

    // Clear aggregate data
    {
        std::lock_guard<std::mutex> lock(aggregateMutex_);
        aggregateIngestionSamples_.clear();
        aggregateDistributionSamples_.clear();
        totalAggregateIngestionBytes_ = 0;
        totalAggregateDistributionBytes_ = 0;
    }
}

// =============================================================================
// Private Helpers
// =============================================================================

double ThroughputOptimizer::calculateCurrentBitrate(
    const std::deque<DataSample>& samples,
    std::chrono::milliseconds windowMs) const {

    if (samples.empty()) {
        return 0.0;
    }

    auto now = std::chrono::steady_clock::now();
    auto windowStart = now - windowMs;

    // Sum bytes within the window
    uint64_t totalBytes = 0;
    for (const auto& sample : samples) {
        if (sample.timestamp >= windowStart) {
            totalBytes += sample.bytes;
        }
    }

    if (totalBytes == 0) {
        return 0.0;
    }

    // For bitrate calculation, we use the configured window duration.
    // This provides stable bitrate estimation:
    // - If data was sent throughout the window, we get accurate bitrate
    // - If data was sent in a burst, we still average over the full window
    //   (which is the expected behavior for smoothing/averaging)
    //
    // Alternative approach would track actual time span, but that leads to
    // unstable readings especially for first few samples.
    double seconds = static_cast<double>(windowMs.count()) / 1000.0;
    double bits = static_cast<double>(totalBytes) * 8.0;
    double mbps = bits / seconds / 1000000.0;

    return mbps;
}

void ThroughputOptimizer::cleanOldSamples(
    std::deque<DataSample>& samples,
    std::chrono::milliseconds windowMs) {

    auto now = std::chrono::steady_clock::now();
    auto windowStart = now - windowMs;

    while (!samples.empty() && samples.front().timestamp < windowStart) {
        samples.pop_front();
    }
}

double ThroughputOptimizer::calculateAverageBitrate(
    uint64_t totalBytes,
    std::chrono::steady_clock::time_point trackingStarted) const {

    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - trackingStarted);

    if (duration.count() <= 0) {
        return 0.0;
    }

    double seconds = static_cast<double>(duration.count()) / 1000.0;
    double bits = static_cast<double>(totalBytes) * 8.0;
    double mbps = bits / seconds / 1000000.0;

    return mbps;
}

void ThroughputOptimizer::checkAndWarn(
    const StreamKey& key,
    double bitrateMbps,
    double limitMbps,
    ThroughputWarningType type,
    std::chrono::steady_clock::time_point& lastWarning) {

    auto now = std::chrono::steady_clock::now();
    ThroughputConfig currentConfig;

    {
        std::shared_lock<std::shared_mutex> configLock(configMutex_);
        currentConfig = config_;
    }

    // Check cooldown
    auto timeSinceLastWarning = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastWarning);

    if (timeSinceLastWarning < currentConfig.warningCooldownMs) {
        return;  // Still in cooldown
    }

    // Check if over limit
    if (bitrateMbps > limitMbps) {
        lastWarning = now;
        invokeWarningCallback(key, bitrateMbps, type);
    }
}

void ThroughputOptimizer::invokeWarningCallback(
    const StreamKey& key,
    double bitrateMbps,
    ThroughputWarningType type) {

    OnThroughputWarningCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = onWarningCallback_;
    }

    if (callback) {
        callback(key, bitrateMbps, type);
    }
}

} // namespace streaming
} // namespace openrtmp
