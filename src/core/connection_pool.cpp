// OpenRTMP - Cross-platform RTMP Server
// Connection Pool Implementation
//
// Implements connection pooling for efficient resource management with:
// - Pre-allocated connection objects to avoid allocation during accept
// - Platform-aware connection limits (1000 desktop, 100 mobile)
// - Rejection with error when limits reached
// - Logging callbacks for capacity planning
//
// Requirements coverage:
// - Requirement 14.1: Desktop platforms support 1000 concurrent TCP connections
// - Requirement 14.2: Mobile platforms support 100 concurrent TCP connections
// - Requirement 14.5: Connection pooling for efficient resource management
// - Requirement 14.6: Reject new connections with error when limits reached

#include "openrtmp/core/connection_pool.hpp"

#include <algorithm>
#include <sstream>

namespace openrtmp {
namespace core {

// =============================================================================
// ConnectionPool Implementation
// =============================================================================

ConnectionPool::ConnectionPool(const ConnectionPoolConfig& config)
    : config_(config)
    , statistics_{}
    , aboveHighWatermark_(false)
    , nextHandle_(1)
{
    // Validate and apply configuration
    if (config_.maxConnections == 0) {
        config_ = ConnectionPoolConfig::platformDefault();
    }

    // Ensure maxConnections is at least 1
    if (config_.maxConnections < 1) {
        config_.maxConnections = 1;
    }

    // Clamp pre-allocation count to max connections
    if (config_.preAllocateCount > config_.maxConnections) {
        config_.preAllocateCount = config_.maxConnections;
    }

    // If pre-allocation not specified, use default percentage
    if (config_.preAllocateCount == 0 && config_.maxConnections > 0) {
        config_.preAllocateCount = config_.maxConnections * DEFAULT_PREALLOCATE_PERCENT / 100;
        if (config_.preAllocateCount == 0) {
            config_.preAllocateCount = 1;  // At least 1
        }
    }

    // Initialize statistics
    statistics_.lastResetTime = std::chrono::steady_clock::now();

    // Pre-allocate connections
    preAllocate();
}

ConnectionPool::~ConnectionPool() {
    clear();
}

ConnectionPool::ConnectionPool(ConnectionPool&& other) noexcept
    : config_(other.config_)
    , entries_(std::move(other.entries_))
    , handleToIndex_(std::move(other.handleToIndex_))
    , availableIndices_(std::move(other.availableIndices_))
    , statistics_(other.statistics_)
    , aboveHighWatermark_(other.aboveHighWatermark_)
    , nextHandle_(other.nextHandle_.load())
{
}

ConnectionPool& ConnectionPool::operator=(ConnectionPool&& other) noexcept {
    if (this != &other) {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        config_ = other.config_;
        entries_ = std::move(other.entries_);
        handleToIndex_ = std::move(other.handleToIndex_);
        availableIndices_ = std::move(other.availableIndices_);
        statistics_ = other.statistics_;
        aboveHighWatermark_ = other.aboveHighWatermark_;
        nextHandle_ = other.nextHandle_.load();
    }
    return *this;
}

void ConnectionPool::preAllocate() {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    entries_.reserve(config_.maxConnections);

    for (size_t i = 0; i < config_.preAllocateCount; ++i) {
        auto entry = allocateEntry();
        if (entry) {
            availableIndices_.push(entries_.size() - 1);
        }
    }

    statistics_.availableConnections = availableIndices_.size();
}

ConnectionPool::ConnectionEntry* ConnectionPool::allocateEntry() {
    // Must be called with lock held
    if (entries_.size() >= config_.maxConnections) {
        return nullptr;
    }

    auto entry = std::make_unique<ConnectionEntry>();
    entry->connection = std::make_unique<Connection>();
    entry->handle = INVALID_CONNECTION_HANDLE;
    entry->inUse = false;

    entries_.push_back(std::move(entry));
    ++statistics_.totalAllocatedConnections;
    ++statistics_.totalAllocations;

    return entries_.back().get();
}

ConnectionHandle ConnectionPool::generateHandle() {
    return nextHandle_.fetch_add(1, std::memory_order_relaxed);
}

Result<ConnectionHandle, Error> ConnectionPool::acquire() {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    ++statistics_.totalAcquires;

    // Check if we have available connections
    if (availableIndices_.empty()) {
        // Try to allocate a new one if under limit
        if (entries_.size() < config_.maxConnections) {
            auto entry = allocateEntry();
            if (entry) {
                availableIndices_.push(entries_.size() - 1);
            }
        }
    }

    // Check if still no available connections
    if (availableIndices_.empty()) {
        ++statistics_.rejectedConnections;

        // Emit limit reached event
        lock.unlock();
        emitLimitEvent(ConnectionLimitEventType::LimitReached,
                       "Connection limit reached: " + std::to_string(config_.maxConnections));
        emitLimitEvent(ConnectionLimitEventType::ConnectionRejected,
                       "Connection rejected due to limit");

        return Result<ConnectionHandle, Error>::error(
            Error(ErrorCode::ConnectionLimitReached,
                  "Connection limit reached (" + std::to_string(config_.maxConnections) + ")",
                  "acquire")
        );
    }

    // Get an available entry
    size_t index = availableIndices_.front();
    availableIndices_.pop();

    ConnectionEntry* entry = entries_[index].get();

    // Reset and prepare the connection
    entry->connection->reset();
    entry->handle = generateHandle();
    entry->inUse = true;
    entry->connection->id = entry->handle;  // Use handle as connection ID

    // Update mapping
    handleToIndex_[entry->handle] = index;

    // Update statistics
    ++statistics_.connectionsInUse;
    statistics_.availableConnections = availableIndices_.size();

    if (statistics_.connectionsInUse > statistics_.peakConnectionsInUse) {
        statistics_.peakConnectionsInUse = statistics_.connectionsInUse;
    }

    // Check watermarks
    size_t currentCount = statistics_.connectionsInUse;
    lock.unlock();
    checkWatermarks(currentCount);

    return Result<ConnectionHandle, Error>::success(entry->handle);
}

void ConnectionPool::release(ConnectionHandle handle) {
    if (handle == INVALID_CONNECTION_HANDLE) {
        return;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Find the entry
    auto it = handleToIndex_.find(handle);
    if (it == handleToIndex_.end()) {
        return;  // Already released or invalid
    }

    size_t index = it->second;
    ConnectionEntry* entry = entries_[index].get();

    if (!entry->inUse) {
        return;  // Already released
    }

    // Reset the connection
    entry->connection->reset();
    entry->inUse = false;

    // Remove from mapping
    handleToIndex_.erase(it);

    // Return to available pool
    availableIndices_.push(index);

    // Update statistics
    --statistics_.connectionsInUse;
    ++statistics_.totalReleases;
    statistics_.availableConnections = availableIndices_.size();

    // Check watermarks
    size_t currentCount = statistics_.connectionsInUse;
    lock.unlock();
    checkWatermarks(currentCount);
}

Connection* ConnectionPool::getConnection(ConnectionHandle handle) {
    if (handle == INVALID_CONNECTION_HANDLE) {
        return nullptr;
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = handleToIndex_.find(handle);
    if (it == handleToIndex_.end()) {
        return nullptr;
    }

    ConnectionEntry* entry = entries_[it->second].get();
    if (!entry->inUse) {
        return nullptr;
    }

    return entry->connection.get();
}

const Connection* ConnectionPool::getConnection(ConnectionHandle handle) const {
    if (handle == INVALID_CONNECTION_HANDLE) {
        return nullptr;
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = handleToIndex_.find(handle);
    if (it == handleToIndex_.end()) {
        return nullptr;
    }

    const ConnectionEntry* entry = entries_[it->second].get();
    if (!entry->inUse) {
        return nullptr;
    }

    return entry->connection.get();
}

ConnectionPoolConfig ConnectionPool::getConfig() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return config_;
}

ConnectionPoolStatistics ConnectionPool::getStatistics() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return statistics_;
}

void ConnectionPool::resetStatistics() {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Keep current counts, reset historical stats
    statistics_.peakConnectionsInUse = statistics_.connectionsInUse;
    statistics_.rejectedConnections = 0;
    statistics_.totalAcquires = 0;
    statistics_.totalReleases = 0;
    statistics_.lastResetTime = std::chrono::steady_clock::now();
}

void ConnectionPool::setConnectionLimitCallback(ConnectionLimitCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    limitCallback_ = std::move(callback);
}

void ConnectionPool::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Reset all entries
    for (auto& entry : entries_) {
        if (entry && entry->inUse) {
            entry->connection->reset();
            entry->inUse = false;
        }
    }

    // Clear mapping
    handleToIndex_.clear();

    // Rebuild available indices
    while (!availableIndices_.empty()) {
        availableIndices_.pop();
    }

    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i]) {
            availableIndices_.push(i);
        }
    }

    // Update statistics
    statistics_.connectionsInUse = 0;
    statistics_.availableConnections = availableIndices_.size();

    // Reset watermark state
    aboveHighWatermark_ = false;
}

void ConnectionPool::checkWatermarks(size_t newCount) {
    size_t highThreshold = config_.maxConnections * config_.highWatermarkPercent / 100;
    size_t lowThreshold = config_.maxConnections * config_.lowWatermarkPercent / 100;

    bool wasAboveHigh = aboveHighWatermark_;
    bool isAboveHigh = newCount >= highThreshold;

    if (isAboveHigh && !wasAboveHigh) {
        // Crossed high watermark going up
        aboveHighWatermark_ = true;
        emitLimitEvent(ConnectionLimitEventType::HighWatermark,
                       "Connection count crossed high watermark: " + std::to_string(newCount) +
                       "/" + std::to_string(config_.maxConnections));
    } else if (!isAboveHigh && wasAboveHigh && newCount <= lowThreshold) {
        // Crossed low watermark going down
        aboveHighWatermark_ = false;
        emitLimitEvent(ConnectionLimitEventType::LowWatermark,
                       "Connection count recovered below low watermark: " + std::to_string(newCount) +
                       "/" + std::to_string(config_.maxConnections));
    }
}

void ConnectionPool::emitLimitEvent(ConnectionLimitEventType type, const std::string& message) {
    ConnectionLimitCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = limitCallback_;
    }

    if (callback) {
        ConnectionLimitEvent event;
        event.type = type;
        event.timestamp = std::chrono::steady_clock::now();
        event.message = message;

        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            event.currentCount = statistics_.connectionsInUse;
            event.maxCount = config_.maxConnections;
        }

        callback(event);
    }
}

} // namespace core
} // namespace openrtmp
