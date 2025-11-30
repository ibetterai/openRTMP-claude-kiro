// OpenRTMP - Cross-platform RTMP Server
// Shutdown Coordinator Implementation
//
// Implements graceful shutdown with:
// - State machine for shutdown phases
// - Publisher notification system
// - Grace period timer
// - Force shutdown capability
// - Statistics tracking and logging

#include "openrtmp/core/shutdown_coordinator.hpp"

#include <sstream>
#include <algorithm>
#include <iostream>

namespace openrtmp {
namespace core {

// =============================================================================
// Constructor and Destructor
// =============================================================================

ShutdownCoordinator::ShutdownCoordinator()
    : state_(ShutdownState::Running)
    , gracePeriodMs_(shutdown::DEFAULT_GRACE_PERIOD_MS)
    , gracePeriodTimerRunning_(false)
    , stats_{}
    , shutdownStartTime_{}
{
}

ShutdownCoordinator::~ShutdownCoordinator() {
    stopGracePeriodTimer();
}

// Move constructor
ShutdownCoordinator::ShutdownCoordinator(ShutdownCoordinator&& other) noexcept
    : state_(other.state_.load())
    , gracePeriodMs_(other.gracePeriodMs_)
    , gracePeriodTimerRunning_(other.gracePeriodTimerRunning_.load())
    , stats_(other.stats_)
    , shutdownStartTime_(other.shutdownStartTime_)
{
    std::unique_lock<std::shared_mutex> lock(other.mutex_);
    publishers_ = std::move(other.publishers_);
    connections_ = std::move(other.connections_);
    activeStreams_ = std::move(other.activeStreams_);

    std::lock_guard<std::mutex> callbackLock(other.callbackMutex_);
    publisherNotificationCallback_ = std::move(other.publisherNotificationCallback_);
    gracePeriodExpiredCallback_ = std::move(other.gracePeriodExpiredCallback_);
    allStreamsEndedCallback_ = std::move(other.allStreamsEndedCallback_);
    forceShutdownCallback_ = std::move(other.forceShutdownCallback_);
    connectionForceTerminateCallback_ = std::move(other.connectionForceTerminateCallback_);
}

// Move assignment
ShutdownCoordinator& ShutdownCoordinator::operator=(ShutdownCoordinator&& other) noexcept {
    if (this != &other) {
        stopGracePeriodTimer();

        state_.store(other.state_.load());
        gracePeriodMs_ = other.gracePeriodMs_;
        gracePeriodTimerRunning_.store(other.gracePeriodTimerRunning_.load());
        stats_ = other.stats_;
        shutdownStartTime_ = other.shutdownStartTime_;

        std::unique_lock<std::shared_mutex> lock(other.mutex_);
        publishers_ = std::move(other.publishers_);
        connections_ = std::move(other.connections_);
        activeStreams_ = std::move(other.activeStreams_);

        std::lock_guard<std::mutex> callbackLock(other.callbackMutex_);
        publisherNotificationCallback_ = std::move(other.publisherNotificationCallback_);
        gracePeriodExpiredCallback_ = std::move(other.gracePeriodExpiredCallback_);
        allStreamsEndedCallback_ = std::move(other.allStreamsEndedCallback_);
        forceShutdownCallback_ = std::move(other.forceShutdownCallback_);
        connectionForceTerminateCallback_ = std::move(other.connectionForceTerminateCallback_);
    }
    return *this;
}

// =============================================================================
// Shutdown Control
// =============================================================================

Result<void, ShutdownError> ShutdownCoordinator::initiateShutdown() {
    ShutdownState expected = ShutdownState::Running;
    if (!state_.compare_exchange_strong(expected, ShutdownState::StoppingNewConnections)) {
        return Result<void, ShutdownError>::error(
            ShutdownError(ShutdownError::Code::AlreadyShuttingDown,
                         "Shutdown already in progress")
        );
    }

    // Record shutdown start time
    shutdownStartTime_ = std::chrono::steady_clock::now();

    // Notify all publishers
    notifyAllPublishers();

    return Result<void, ShutdownError>::success();
}

Result<void, ShutdownError> ShutdownCoordinator::forceShutdown() {
    ShutdownState current = state_.load();

    if (current == ShutdownState::ForceTerminating ||
        current == ShutdownState::Complete) {
        return Result<void, ShutdownError>::error(
            ShutdownError(ShutdownError::Code::AlreadyShuttingDown,
                         "Shutdown already in progress or complete")
        );
    }

    // Stop grace period timer if running
    stopGracePeriodTimer();

    // Record shutdown start time if not already started
    if (current == ShutdownState::Running) {
        shutdownStartTime_ = std::chrono::steady_clock::now();
    }

    state_.store(ShutdownState::ForceTerminating);

    // Invoke force shutdown callback
    std::lock_guard<std::mutex> callbackLock(callbackMutex_);
    safeInvokeCallback(forceShutdownCallback_);

    return Result<void, ShutdownError>::success();
}

void ShutdownCoordinator::startGracePeriod() {
    if (gracePeriodTimerRunning_) {
        return;
    }

    gracePeriodTimerRunning_ = true;

    // Start grace period timer thread
    gracePeriodThread_ = std::thread(&ShutdownCoordinator::gracePeriodTimerThread, this);
}

void ShutdownCoordinator::terminateAllConnections() {
    std::set<ConnectionId> connectionsToTerminate;

    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        connectionsToTerminate = connections_;
    }

    std::lock_guard<std::mutex> callbackLock(callbackMutex_);
    for (ConnectionId connId : connectionsToTerminate) {
        safeInvokeCallback(connectionForceTerminateCallback_, connId);
    }
}

// =============================================================================
// State Accessors
// =============================================================================

ShutdownState ShutdownCoordinator::state() const {
    return state_.load();
}

bool ShutdownCoordinator::isShuttingDown() const {
    ShutdownState current = state_.load();
    return current != ShutdownState::Running;
}

bool ShutdownCoordinator::isShutdownComplete() const {
    return state_.load() == ShutdownState::Complete;
}

bool ShutdownCoordinator::canAcceptConnection() const {
    return state_.load() == ShutdownState::Running;
}

// =============================================================================
// State Transitions
// =============================================================================

void ShutdownCoordinator::notifyPublishersComplete() {
    ShutdownState expected = ShutdownState::StoppingNewConnections;
    state_.compare_exchange_strong(expected, ShutdownState::NotifyingPublishers);
}

void ShutdownCoordinator::gracePeriodComplete() {
    ShutdownState expected = ShutdownState::NotifyingPublishers;
    state_.compare_exchange_strong(expected, ShutdownState::GracePeriod);
}

void ShutdownCoordinator::forceTerminationComplete() {
    ShutdownState expected = ShutdownState::GracePeriod;
    state_.compare_exchange_strong(expected, ShutdownState::ForceTerminating);
}

void ShutdownCoordinator::shutdownComplete() {
    state_.store(ShutdownState::Complete);

    // Calculate shutdown duration
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - shutdownStartTime_
    );

    std::unique_lock<std::shared_mutex> lock(mutex_);
    stats_.shutdownDurationMs = static_cast<uint32_t>(duration.count());
}

// =============================================================================
// Publisher Management
// =============================================================================

void ShutdownCoordinator::registerPublisher(PublisherId publisherId) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    publishers_.insert(publisherId);
}

void ShutdownCoordinator::unregisterPublisher(PublisherId publisherId) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    publishers_.erase(publisherId);
}

size_t ShutdownCoordinator::publisherCount() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return publishers_.size();
}

// =============================================================================
// Connection Management
// =============================================================================

void ShutdownCoordinator::registerConnection(ConnectionId connectionId) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    connections_.insert(connectionId);
}

void ShutdownCoordinator::unregisterConnection(ConnectionId connectionId) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    connections_.erase(connectionId);
}

size_t ShutdownCoordinator::connectionCount() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return connections_.size();
}

// =============================================================================
// Active Stream Management
// =============================================================================

void ShutdownCoordinator::registerActiveStream(const std::string& streamKey) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    activeStreams_.insert(streamKey);
}

void ShutdownCoordinator::unregisterActiveStream(const std::string& streamKey) {
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        activeStreams_.erase(streamKey);
    }

    // Check if all streams have ended during shutdown
    if (isShuttingDown()) {
        checkAllStreamsEnded();
    }
}

size_t ShutdownCoordinator::activeStreamCount() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return activeStreams_.size();
}

// =============================================================================
// Configuration
// =============================================================================

uint32_t ShutdownCoordinator::gracePeriodMs() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return gracePeriodMs_;
}

void ShutdownCoordinator::setGracePeriodMs(uint32_t ms) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    gracePeriodMs_ = std::clamp(ms,
                                shutdown::MIN_GRACE_PERIOD_MS,
                                shutdown::MAX_GRACE_PERIOD_MS);
}

// =============================================================================
// Statistics
// =============================================================================

void ShutdownCoordinator::recordConnectionTermination(bool graceful) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    stats_.totalConnectionsTerminated++;
    if (graceful) {
        stats_.gracefulDisconnects++;
    } else {
        stats_.forcedDisconnects++;
    }
}

void ShutdownCoordinator::recordStreamInterruption() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    stats_.totalStreamsInterrupted++;
}

ShutdownStatistics ShutdownCoordinator::getShutdownStatistics() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return stats_;
}

std::string ShutdownCoordinator::getShutdownSummary() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::ostringstream oss;
    oss << "Shutdown Summary:\n";
    oss << "  State: " << shutdownStateToString(state_.load()) << "\n";
    oss << "  Total connections terminated: " << stats_.totalConnectionsTerminated << "\n";
    oss << "  Graceful disconnects: " << stats_.gracefulDisconnects << "\n";
    oss << "  Forced disconnects: " << stats_.forcedDisconnects << "\n";
    oss << "  Total streams interrupted: " << stats_.totalStreamsInterrupted << "\n";
    oss << "  Shutdown duration: " << stats_.shutdownDurationMs << " ms\n";
    oss << "  Remaining publishers: " << publishers_.size() << "\n";
    oss << "  Remaining connections: " << connections_.size() << "\n";
    oss << "  Remaining active streams: " << activeStreams_.size() << "\n";

    return oss.str();
}

// =============================================================================
// Callbacks
// =============================================================================

void ShutdownCoordinator::setPublisherNotificationCallback(PublisherNotificationCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    publisherNotificationCallback_ = std::move(callback);
}

void ShutdownCoordinator::setGracePeriodExpiredCallback(GracePeriodExpiredCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    gracePeriodExpiredCallback_ = std::move(callback);
}

void ShutdownCoordinator::setAllStreamsEndedCallback(AllStreamsEndedCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    allStreamsEndedCallback_ = std::move(callback);
}

void ShutdownCoordinator::setForceShutdownCallback(ForceShutdownCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    forceShutdownCallback_ = std::move(callback);
}

void ShutdownCoordinator::setConnectionForceTerminateCallback(ConnectionForceTerminateCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    connectionForceTerminateCallback_ = std::move(callback);
}

// =============================================================================
// Internal Methods
// =============================================================================

void ShutdownCoordinator::notifyAllPublishers() {
    std::set<PublisherId> publishersToNotify;

    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        publishersToNotify = publishers_;
    }

    std::lock_guard<std::mutex> callbackLock(callbackMutex_);
    for (PublisherId publisherId : publishersToNotify) {
        safeInvokeCallback(publisherNotificationCallback_, publisherId);
    }
}

void ShutdownCoordinator::checkAllStreamsEnded() {
    bool allEnded = false;

    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        allEnded = activeStreams_.empty();
    }

    if (allEnded) {
        // Stop grace period timer early
        stopGracePeriodTimer();

        // Notify that all streams have ended
        std::lock_guard<std::mutex> callbackLock(callbackMutex_);
        safeInvokeCallback(allStreamsEndedCallback_);
    }
}

void ShutdownCoordinator::gracePeriodTimerThread() {
    uint32_t periodMs;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        periodMs = gracePeriodMs_;
    }

    std::unique_lock<std::mutex> lock(gracePeriodMutex_);
    auto status = gracePeriodCv_.wait_for(
        lock,
        std::chrono::milliseconds(periodMs),
        [this]() { return !gracePeriodTimerRunning_.load(); }
    );

    // If we timed out (not cancelled), invoke the callback
    if (!status && gracePeriodTimerRunning_) {
        gracePeriodTimerRunning_ = false;

        std::lock_guard<std::mutex> callbackLock(callbackMutex_);
        safeInvokeCallback(gracePeriodExpiredCallback_);
    }
}

void ShutdownCoordinator::stopGracePeriodTimer() {
    // Signal the timer to stop
    gracePeriodTimerRunning_.store(false);

    // Wake up the timer thread if it's waiting
    {
        std::lock_guard<std::mutex> lock(gracePeriodMutex_);
    }
    gracePeriodCv_.notify_all();

    // Always try to join the thread if it's joinable
    if (gracePeriodThread_.joinable()) {
        gracePeriodThread_.join();
    }
}

template<typename Callback, typename... Args>
void ShutdownCoordinator::safeInvokeCallback(const Callback& callback, Args&&... args) {
    if (callback) {
        try {
            callback(std::forward<Args>(args)...);
        } catch (const std::exception& e) {
            // Log exception but don't propagate
            // In production, this would use the structured logger
            std::cerr << "Exception in shutdown callback: " << e.what() << std::endl;
        } catch (...) {
            // Catch all other exceptions
            std::cerr << "Unknown exception in shutdown callback" << std::endl;
        }
    }
}

} // namespace core
} // namespace openrtmp
