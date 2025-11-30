// OpenRTMP - Cross-platform RTMP Server
// Metrics Collection Component Implementation
//
// Requirements Covered:
// - 12.6: Expose latency metrics through API (ingestion/distribution delays)
// - 13.5: Provide throughput metrics through API (bitrate per stream)
// - 18.3: Expose metrics through programmatic API (connections, streams, bandwidth, errors)

#include "openrtmp/core/metrics_collector.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace openrtmp {
namespace core {

std::string errorCategoryToString(ErrorCategory category) {
    switch (category) {
        case ErrorCategory::Protocol:
            return "protocol";
        case ErrorCategory::Network:
            return "network";
        case ErrorCategory::Authentication:
            return "authentication";
        case ErrorCategory::Resource:
            return "resource";
        case ErrorCategory::Internal:
            return "internal";
        default:
            return "unknown";
    }
}

MetricsCollector::MetricsCollector()
    : startTime_(std::chrono::steady_clock::now()) {
    // Initialize error counts for all categories
    errorCounts_[ErrorCategory::Protocol].store(0);
    errorCounts_[ErrorCategory::Network].store(0);
    errorCounts_[ErrorCategory::Authentication].store(0);
    errorCounts_[ErrorCategory::Resource].store(0);
    errorCounts_[ErrorCategory::Internal].store(0);
}

MetricsCollector::~MetricsCollector() = default;

// =============================================================================
// Connection Metrics
// =============================================================================

void MetricsCollector::incrementConnections() {
    activeConnections_.fetch_add(1, std::memory_order_relaxed);
    totalConnections_.fetch_add(1, std::memory_order_relaxed);
    updatePeakConnections();
}

void MetricsCollector::decrementConnections() {
    uint64_t current = activeConnections_.load(std::memory_order_relaxed);
    if (current > 0) {
        activeConnections_.fetch_sub(1, std::memory_order_relaxed);
    }
}

uint64_t MetricsCollector::getActiveConnections() const {
    return activeConnections_.load(std::memory_order_relaxed);
}

void MetricsCollector::updatePeakConnections() {
    uint64_t current = activeConnections_.load(std::memory_order_relaxed);
    uint64_t peak = peakConnections_.load(std::memory_order_relaxed);
    while (current > peak) {
        if (peakConnections_.compare_exchange_weak(peak, current,
                                                    std::memory_order_relaxed)) {
            break;
        }
    }
}

// =============================================================================
// Stream Metrics
// =============================================================================

void MetricsCollector::incrementStreams() {
    activeStreams_.fetch_add(1, std::memory_order_relaxed);
    totalStreams_.fetch_add(1, std::memory_order_relaxed);
    updatePeakStreams();
}

void MetricsCollector::decrementStreams() {
    uint64_t current = activeStreams_.load(std::memory_order_relaxed);
    if (current > 0) {
        activeStreams_.fetch_sub(1, std::memory_order_relaxed);
    }
}

uint64_t MetricsCollector::getActiveStreams() const {
    return activeStreams_.load(std::memory_order_relaxed);
}

void MetricsCollector::updatePeakStreams() {
    uint64_t current = activeStreams_.load(std::memory_order_relaxed);
    uint64_t peak = peakStreams_.load(std::memory_order_relaxed);
    while (current > peak) {
        if (peakStreams_.compare_exchange_weak(peak, current,
                                                std::memory_order_relaxed)) {
            break;
        }
    }
}

// =============================================================================
// Bandwidth Metrics
// =============================================================================

void MetricsCollector::addBytesIn(uint64_t bytes) {
    totalBytesIn_.fetch_add(bytes, std::memory_order_relaxed);
}

void MetricsCollector::addBytesOut(uint64_t bytes) {
    totalBytesOut_.fetch_add(bytes, std::memory_order_relaxed);
}

uint64_t MetricsCollector::getTotalBytesIn() const {
    return totalBytesIn_.load(std::memory_order_relaxed);
}

uint64_t MetricsCollector::getTotalBytesOut() const {
    return totalBytesOut_.load(std::memory_order_relaxed);
}

void MetricsCollector::setCurrentBandwidthIn(uint64_t bps) {
    currentBandwidthIn_.store(bps, std::memory_order_relaxed);
}

void MetricsCollector::setCurrentBandwidthOut(uint64_t bps) {
    currentBandwidthOut_.store(bps, std::memory_order_relaxed);
}

uint64_t MetricsCollector::getCurrentBandwidthIn() const {
    return currentBandwidthIn_.load(std::memory_order_relaxed);
}

uint64_t MetricsCollector::getCurrentBandwidthOut() const {
    return currentBandwidthOut_.load(std::memory_order_relaxed);
}

// =============================================================================
// Error Metrics
// =============================================================================

void MetricsCollector::recordError(ErrorCategory category) {
    std::lock_guard<std::mutex> lock(errorMutex_);
    errorCounts_[category].fetch_add(1, std::memory_order_relaxed);
}

uint64_t MetricsCollector::getErrorCount(ErrorCategory category) const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    auto it = errorCounts_.find(category);
    if (it != errorCounts_.end()) {
        return it->second.load(std::memory_order_relaxed);
    }
    return 0;
}

uint64_t MetricsCollector::getTotalErrorCount() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    uint64_t total = 0;
    for (const auto& [category, count] : errorCounts_) {
        total += count.load(std::memory_order_relaxed);
    }
    return total;
}

// =============================================================================
// Per-Stream Metrics
// =============================================================================

void MetricsCollector::createStreamMetrics(const StreamKey& key) {
    std::lock_guard<std::mutex> lock(streamMetricsMutex_);
    if (streamMetrics_.find(key) == streamMetrics_.end()) {
        auto metrics = std::make_shared<StreamMetricsData>();
        metrics->key = key;
        metrics->startTime = std::chrono::steady_clock::now();
        streamMetrics_[key] = metrics;
        streamDelaySamples_[key] = StreamDelaySamples{};
    }
}

void MetricsCollector::removeStreamMetrics(const StreamKey& key) {
    std::lock_guard<std::mutex> lock(streamMetricsMutex_);
    streamMetrics_.erase(key);
    streamDelaySamples_.erase(key);
}

std::optional<StreamMetricsData> MetricsCollector::getStreamMetrics(const StreamKey& key) const {
    std::lock_guard<std::mutex> lock(streamMetricsMutex_);
    auto it = streamMetrics_.find(key);
    if (it != streamMetrics_.end()) {
        return *it->second;
    }
    return std::nullopt;
}

std::unordered_map<StreamKey, StreamMetricsData> MetricsCollector::getAllStreamMetrics() const {
    std::lock_guard<std::mutex> lock(streamMetricsMutex_);
    std::unordered_map<StreamKey, StreamMetricsData> result;
    for (const auto& [key, metricsPtr] : streamMetrics_) {
        result[key] = *metricsPtr;
    }
    return result;
}

void MetricsCollector::recordIngestionDelay(const StreamKey& key, std::chrono::milliseconds delay) {
    std::lock_guard<std::mutex> lock(streamMetricsMutex_);
    auto it = streamMetrics_.find(key);
    if (it != streamMetrics_.end()) {
        auto& samples = streamDelaySamples_[key].ingestionDelays;
        samples.push_back(delay);

        // Keep only recent samples (last 100)
        if (samples.size() > 100) {
            samples.erase(samples.begin());
        }

        // Calculate average
        if (!samples.empty()) {
            int64_t sum = 0;
            for (const auto& s : samples) {
                sum += s.count();
            }
            it->second->averageIngestionDelay =
                std::chrono::milliseconds(sum / static_cast<int64_t>(samples.size()));
        }
    }
}

void MetricsCollector::recordDistributionDelay(const StreamKey& key, std::chrono::milliseconds delay) {
    std::lock_guard<std::mutex> lock(streamMetricsMutex_);
    auto it = streamMetrics_.find(key);
    if (it != streamMetrics_.end()) {
        auto& samples = streamDelaySamples_[key].distributionDelays;
        samples.push_back(delay);

        // Keep only recent samples (last 100)
        if (samples.size() > 100) {
            samples.erase(samples.begin());
        }

        // Calculate average
        if (!samples.empty()) {
            int64_t sum = 0;
            for (const auto& s : samples) {
                sum += s.count();
            }
            it->second->averageDistributionDelay =
                std::chrono::milliseconds(sum / static_cast<int64_t>(samples.size()));
        }
    }
}

void MetricsCollector::updateStreamBitrate(const StreamKey& key, uint64_t bitrate) {
    std::lock_guard<std::mutex> lock(streamMetricsMutex_);
    auto it = streamMetrics_.find(key);
    if (it != streamMetrics_.end()) {
        it->second->currentBitrate = bitrate;
    }
}

void MetricsCollector::addStreamBytesIn(const StreamKey& key, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(streamMetricsMutex_);
    auto it = streamMetrics_.find(key);
    if (it != streamMetrics_.end()) {
        it->second->bytesIn += bytes;
    }
}

void MetricsCollector::addStreamBytesOut(const StreamKey& key, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(streamMetricsMutex_);
    auto it = streamMetrics_.find(key);
    if (it != streamMetrics_.end()) {
        it->second->bytesOut += bytes;
    }
}

void MetricsCollector::incrementStreamSubscribers(const StreamKey& key) {
    std::lock_guard<std::mutex> lock(streamMetricsMutex_);
    auto it = streamMetrics_.find(key);
    if (it != streamMetrics_.end()) {
        it->second->subscriberCount++;
    }
}

void MetricsCollector::decrementStreamSubscribers(const StreamKey& key) {
    std::lock_guard<std::mutex> lock(streamMetricsMutex_);
    auto it = streamMetrics_.find(key);
    if (it != streamMetrics_.end() && it->second->subscriberCount > 0) {
        it->second->subscriberCount--;
    }
}

// =============================================================================
// Histogram Metrics
// =============================================================================

void MetricsCollector::recordLatency(std::chrono::milliseconds latency) {
    std::lock_guard<std::mutex> lock(latencyMutex_);
    latencySamples_.push_back(latency);

    // Keep maximum 10000 samples for memory efficiency
    if (latencySamples_.size() > 10000) {
        latencySamples_.erase(latencySamples_.begin());
    }
}

LatencyHistogram MetricsCollector::getLatencyHistogram() const {
    std::lock_guard<std::mutex> lock(latencyMutex_);
    LatencyHistogram histogram;

    if (latencySamples_.empty()) {
        return histogram;
    }

    // Copy and sort samples
    std::vector<std::chrono::milliseconds> sorted = latencySamples_;
    std::sort(sorted.begin(), sorted.end());

    histogram.totalCount = sorted.size();
    histogram.min = sorted.front();
    histogram.max = sorted.back();

    // Calculate percentiles
    histogram.p50 = calculatePercentile(sorted, 0.50);
    histogram.p95 = calculatePercentile(sorted, 0.95);
    histogram.p99 = calculatePercentile(sorted, 0.99);

    // Create buckets (0-10ms, 10-25ms, 25-50ms, 50-100ms, 100-250ms, 250-500ms, 500ms+)
    std::vector<int64_t> bucketBounds = {10, 25, 50, 100, 250, 500, INT64_MAX};
    std::vector<uint64_t> bucketCounts(bucketBounds.size(), 0);

    for (const auto& sample : sorted) {
        for (size_t i = 0; i < bucketBounds.size(); ++i) {
            if (sample.count() <= bucketBounds[i]) {
                bucketCounts[i]++;
                break;
            }
        }
    }

    for (size_t i = 0; i < bucketBounds.size(); ++i) {
        histogram.buckets.emplace_back(
            std::chrono::milliseconds(bucketBounds[i]),
            bucketCounts[i]
        );
    }

    return histogram;
}

std::chrono::milliseconds MetricsCollector::calculatePercentile(
    const std::vector<std::chrono::milliseconds>& samples,
    double percentile) const {
    if (samples.empty()) {
        return std::chrono::milliseconds(0);
    }

    size_t index = static_cast<size_t>(std::floor(percentile * static_cast<double>(samples.size() - 1)));
    return samples[index];
}

void MetricsCollector::recordBitrate(uint64_t bitrate) {
    std::lock_guard<std::mutex> lock(bitrateMutex_);
    bitrateSamples_.push_back(bitrate);

    // Keep maximum 10000 samples
    if (bitrateSamples_.size() > 10000) {
        bitrateSamples_.erase(bitrateSamples_.begin());
    }
}

BitrateHistogram MetricsCollector::getBitrateHistogram() const {
    std::lock_guard<std::mutex> lock(bitrateMutex_);
    BitrateHistogram histogram;

    if (bitrateSamples_.empty()) {
        return histogram;
    }

    // Copy and sort samples
    std::vector<uint64_t> sorted = bitrateSamples_;
    std::sort(sorted.begin(), sorted.end());

    histogram.totalCount = sorted.size();
    histogram.min = sorted.front();
    histogram.max = sorted.back();

    // Calculate average
    uint64_t sum = 0;
    for (const auto& s : sorted) {
        sum += s;
    }
    histogram.average = sum / sorted.size();

    // Create buckets (1Mbps, 2Mbps, 5Mbps, 10Mbps, 20Mbps, 50Mbps+)
    std::vector<uint64_t> bucketBounds = {
        1000000, 2000000, 5000000, 10000000, 20000000, 50000000, UINT64_MAX
    };
    std::vector<uint64_t> bucketCounts(bucketBounds.size(), 0);

    for (const auto& sample : sorted) {
        for (size_t i = 0; i < bucketBounds.size(); ++i) {
            if (sample <= bucketBounds[i]) {
                bucketCounts[i]++;
                break;
            }
        }
    }

    for (size_t i = 0; i < bucketBounds.size(); ++i) {
        histogram.buckets.emplace_back(bucketBounds[i], bucketCounts[i]);
    }

    return histogram;
}

// =============================================================================
// Snapshot and Export
// =============================================================================

MetricsSnapshot MetricsCollector::takeSnapshot() const {
    MetricsSnapshot snapshot;

    // Connection metrics
    snapshot.activeConnections = activeConnections_.load(std::memory_order_relaxed);
    snapshot.peakConnections = peakConnections_.load(std::memory_order_relaxed);
    snapshot.totalConnections = totalConnections_.load(std::memory_order_relaxed);

    // Stream metrics
    snapshot.activeStreams = activeStreams_.load(std::memory_order_relaxed);
    snapshot.peakStreams = peakStreams_.load(std::memory_order_relaxed);
    snapshot.totalStreams = totalStreams_.load(std::memory_order_relaxed);

    // Bandwidth metrics
    snapshot.totalBytesIn = totalBytesIn_.load(std::memory_order_relaxed);
    snapshot.totalBytesOut = totalBytesOut_.load(std::memory_order_relaxed);
    snapshot.currentBandwidthIn = currentBandwidthIn_.load(std::memory_order_relaxed);
    snapshot.currentBandwidthOut = currentBandwidthOut_.load(std::memory_order_relaxed);

    // Error metrics
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        uint64_t total = 0;
        for (const auto& [category, count] : errorCounts_) {
            uint64_t c = count.load(std::memory_order_relaxed);
            snapshot.errorsByCategory[category] = c;
            total += c;
        }
        snapshot.totalErrors = total;
    }

    // Timing
    snapshot.uptime = getUptime();
    snapshot.snapshotTime = std::chrono::system_clock::now();

    return snapshot;
}

void MetricsCollector::reset() {
    activeConnections_.store(0, std::memory_order_relaxed);
    peakConnections_.store(0, std::memory_order_relaxed);
    totalConnections_.store(0, std::memory_order_relaxed);

    activeStreams_.store(0, std::memory_order_relaxed);
    peakStreams_.store(0, std::memory_order_relaxed);
    totalStreams_.store(0, std::memory_order_relaxed);

    totalBytesIn_.store(0, std::memory_order_relaxed);
    totalBytesOut_.store(0, std::memory_order_relaxed);
    currentBandwidthIn_.store(0, std::memory_order_relaxed);
    currentBandwidthOut_.store(0, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        for (auto& [category, count] : errorCounts_) {
            count.store(0, std::memory_order_relaxed);
        }
    }

    {
        std::lock_guard<std::mutex> lock(latencyMutex_);
        latencySamples_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(bitrateMutex_);
        bitrateSamples_.clear();
    }
}

std::chrono::milliseconds MetricsCollector::getUptime() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime_);
}

std::string MetricsCollector::exportPrometheus() const {
    std::ostringstream oss;

    auto snapshot = takeSnapshot();

    // Connection metrics
    oss << "# HELP openrtmp_connections_active Current number of active connections\n";
    oss << "# TYPE openrtmp_connections_active gauge\n";
    oss << "openrtmp_connections_active " << snapshot.activeConnections << "\n";

    oss << "# HELP openrtmp_connections_peak Peak number of connections\n";
    oss << "# TYPE openrtmp_connections_peak gauge\n";
    oss << "openrtmp_connections_peak " << snapshot.peakConnections << "\n";

    oss << "# HELP openrtmp_connections_total Total connections since start\n";
    oss << "# TYPE openrtmp_connections_total counter\n";
    oss << "openrtmp_connections_total " << snapshot.totalConnections << "\n";

    // Stream metrics
    oss << "# HELP openrtmp_streams_active Current number of active streams\n";
    oss << "# TYPE openrtmp_streams_active gauge\n";
    oss << "openrtmp_streams_active " << snapshot.activeStreams << "\n";

    oss << "# HELP openrtmp_streams_peak Peak number of streams\n";
    oss << "# TYPE openrtmp_streams_peak gauge\n";
    oss << "openrtmp_streams_peak " << snapshot.peakStreams << "\n";

    // Bandwidth metrics
    oss << "# HELP openrtmp_bytes_in_total Total bytes received\n";
    oss << "# TYPE openrtmp_bytes_in_total counter\n";
    oss << "openrtmp_bytes_in_total " << snapshot.totalBytesIn << "\n";

    oss << "# HELP openrtmp_bytes_out_total Total bytes sent\n";
    oss << "# TYPE openrtmp_bytes_out_total counter\n";
    oss << "openrtmp_bytes_out_total " << snapshot.totalBytesOut << "\n";

    oss << "# HELP openrtmp_bandwidth_in_bps Current ingestion bandwidth (bps)\n";
    oss << "# TYPE openrtmp_bandwidth_in_bps gauge\n";
    oss << "openrtmp_bandwidth_in_bps " << snapshot.currentBandwidthIn << "\n";

    oss << "# HELP openrtmp_bandwidth_out_bps Current distribution bandwidth (bps)\n";
    oss << "# TYPE openrtmp_bandwidth_out_bps gauge\n";
    oss << "openrtmp_bandwidth_out_bps " << snapshot.currentBandwidthOut << "\n";

    // Error metrics
    oss << "# HELP openrtmp_errors_total Total errors by category\n";
    oss << "# TYPE openrtmp_errors_total counter\n";
    for (const auto& [category, count] : snapshot.errorsByCategory) {
        oss << "openrtmp_errors_total{category=\"" << errorCategoryToString(category)
            << "\"} " << count << "\n";
    }

    // Uptime
    oss << "# HELP openrtmp_uptime_seconds Server uptime in seconds\n";
    oss << "# TYPE openrtmp_uptime_seconds gauge\n";
    oss << "openrtmp_uptime_seconds " << (snapshot.uptime.count() / 1000.0) << "\n";

    return oss.str();
}

std::string MetricsCollector::exportJson() const {
    auto snapshot = takeSnapshot();

    std::ostringstream oss;
    oss << "{";
    oss << "\"activeConnections\":" << snapshot.activeConnections << ",";
    oss << "\"peakConnections\":" << snapshot.peakConnections << ",";
    oss << "\"totalConnections\":" << snapshot.totalConnections << ",";
    oss << "\"activeStreams\":" << snapshot.activeStreams << ",";
    oss << "\"peakStreams\":" << snapshot.peakStreams << ",";
    oss << "\"totalStreams\":" << snapshot.totalStreams << ",";
    oss << "\"totalBytesIn\":" << snapshot.totalBytesIn << ",";
    oss << "\"totalBytesOut\":" << snapshot.totalBytesOut << ",";
    oss << "\"currentBandwidthIn\":" << snapshot.currentBandwidthIn << ",";
    oss << "\"currentBandwidthOut\":" << snapshot.currentBandwidthOut << ",";
    oss << "\"totalErrors\":" << snapshot.totalErrors << ",";
    oss << "\"errorsByCategory\":{";

    bool first = true;
    for (const auto& [category, count] : snapshot.errorsByCategory) {
        if (!first) oss << ",";
        oss << "\"" << errorCategoryToString(category) << "\":" << count;
        first = false;
    }
    oss << "},";

    oss << "\"uptimeMs\":" << snapshot.uptime.count();
    oss << "}";

    return oss.str();
}

} // namespace core
} // namespace openrtmp
