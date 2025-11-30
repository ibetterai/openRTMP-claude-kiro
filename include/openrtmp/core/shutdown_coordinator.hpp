// OpenRTMP - Cross-platform RTMP Server
// Shutdown Coordinator - Graceful shutdown management
//
// Responsibilities:
// - Stop accepting new connections immediately on shutdown signal
// - Notify connected publishers of impending shutdown
// - Allow active streams up to 30 seconds grace period to complete
// - Force terminate remaining connections after grace period
// - Support force-shutdown option for immediate termination
// - Log shutdown summary with connection and stream counts
//
// Requirements coverage:
// - Requirement 19.1: Stop accepting new connections immediately on shutdown
// - Requirement 19.2: Notify all connected publishers of impending shutdown
// - Requirement 19.3: Allow active streams up to 30 seconds grace period
// - Requirement 19.4: Force terminate remaining connections after grace period
// - Requirement 19.5: Support force-shutdown option for immediate termination
// - Requirement 19.6: Log shutdown summary with connection and stream counts

#ifndef OPENRTMP_CORE_SHUTDOWN_COORDINATOR_HPP
#define OPENRTMP_CORE_SHUTDOWN_COORDINATOR_HPP

#include <cstdint>
#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <set>
#include <unordered_set>
#include <chrono>
#include <thread>
#include <condition_variable>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/result.hpp"

namespace openrtmp {
namespace core {

// =============================================================================
// Constants
// =============================================================================

namespace shutdown {
    /// Default grace period in milliseconds (30 seconds per Requirement 19.3)
    constexpr uint32_t DEFAULT_GRACE_PERIOD_MS = 30000;

    /// Minimum grace period allowed (10 milliseconds for testing)
    constexpr uint32_t MIN_GRACE_PERIOD_MS = 10;

    /// Maximum grace period allowed (5 minutes)
    constexpr uint32_t MAX_GRACE_PERIOD_MS = 300000;
}

// =============================================================================
// Shutdown State Enumeration
// =============================================================================

/**
 * @brief Shutdown state enumeration.
 *
 * Represents the current state of the server shutdown process.
 * States progress linearly during graceful shutdown.
 */
enum class ShutdownState {
    Running,                ///< Server is running normally
    StoppingNewConnections, ///< No longer accepting new connections (Req 19.1)
    NotifyingPublishers,    ///< Notifying publishers of shutdown (Req 19.2)
    GracePeriod,            ///< Waiting for active streams to complete (Req 19.3)
    ForceTerminating,       ///< Force terminating remaining connections (Req 19.4)
    Complete                ///< Shutdown complete
};

/**
 * @brief Convert shutdown state to string representation.
 *
 * @param state The shutdown state
 * @return String representation of the state
 */
inline const char* shutdownStateToString(ShutdownState state) {
    switch (state) {
        case ShutdownState::Running:                return "Running";
        case ShutdownState::StoppingNewConnections: return "StoppingNewConnections";
        case ShutdownState::NotifyingPublishers:    return "NotifyingPublishers";
        case ShutdownState::GracePeriod:            return "GracePeriod";
        case ShutdownState::ForceTerminating:       return "ForceTerminating";
        case ShutdownState::Complete:               return "Complete";
        default:                                    return "Unknown";
    }
}

// =============================================================================
// Shutdown Error Types
// =============================================================================

/**
 * @brief Shutdown error information.
 */
struct ShutdownError {
    /**
     * @brief Error codes for shutdown operations.
     */
    enum class Code {
        AlreadyShuttingDown,    ///< Shutdown already in progress
        InvalidState,           ///< Invalid state for operation
        InternalError           ///< Internal error occurred
    };

    Code code;                  ///< Error code
    std::string message;        ///< Human-readable error message

    ShutdownError(Code c = Code::InternalError, std::string msg = "")
        : code(c), message(std::move(msg)) {}
};

// =============================================================================
// Shutdown Statistics
// =============================================================================

/**
 * @brief Statistics about the shutdown process (Requirement 19.6).
 */
struct ShutdownStatistics {
    uint32_t totalConnectionsTerminated;    ///< Total connections terminated
    uint32_t totalStreamsInterrupted;       ///< Total streams interrupted
    uint32_t gracefulDisconnects;           ///< Connections that disconnected gracefully
    uint32_t forcedDisconnects;             ///< Connections that were force terminated
    uint32_t shutdownDurationMs;            ///< Time taken to complete shutdown

    ShutdownStatistics()
        : totalConnectionsTerminated(0)
        , totalStreamsInterrupted(0)
        , gracefulDisconnects(0)
        , forcedDisconnects(0)
        , shutdownDurationMs(0)
    {}
};

// =============================================================================
// Callback Types
// =============================================================================

/**
 * @brief Callback to notify a publisher of impending shutdown (Requirement 19.2).
 * @param publisherId The publisher to notify
 */
using PublisherNotificationCallback = std::function<void(PublisherId publisherId)>;

/**
 * @brief Callback when grace period expires (Requirement 19.3, 19.4).
 */
using GracePeriodExpiredCallback = std::function<void()>;

/**
 * @brief Callback when all streams have ended during grace period.
 */
using AllStreamsEndedCallback = std::function<void()>;

/**
 * @brief Callback for force shutdown (Requirement 19.5).
 */
using ForceShutdownCallback = std::function<void()>;

/**
 * @brief Callback to force terminate a connection.
 * @param connectionId The connection to terminate
 */
using ConnectionForceTerminateCallback = std::function<void(ConnectionId connectionId)>;

// =============================================================================
// Shutdown Coordinator Interface
// =============================================================================

/**
 * @brief Interface for shutdown coordination operations.
 */
class IShutdownCoordinator {
public:
    virtual ~IShutdownCoordinator() = default;

    // -------------------------------------------------------------------------
    // Shutdown Control
    // -------------------------------------------------------------------------

    /**
     * @brief Initiate graceful shutdown (Requirement 19.1-19.4).
     *
     * Starts the graceful shutdown process:
     * 1. Stop accepting new connections
     * 2. Notify publishers
     * 3. Wait for grace period
     * 4. Force terminate remaining connections
     *
     * @return Result indicating success or error
     */
    virtual Result<void, ShutdownError> initiateShutdown() = 0;

    /**
     * @brief Force immediate shutdown (Requirement 19.5).
     *
     * Immediately terminates all connections without grace period.
     *
     * @return Result indicating success or error
     */
    virtual Result<void, ShutdownError> forceShutdown() = 0;

    /**
     * @brief Start the grace period countdown.
     *
     * Called after publishers have been notified.
     */
    virtual void startGracePeriod() = 0;

    /**
     * @brief Terminate all remaining connections.
     *
     * Called after grace period expires or during force shutdown.
     */
    virtual void terminateAllConnections() = 0;

    // -------------------------------------------------------------------------
    // State Accessors
    // -------------------------------------------------------------------------

    /**
     * @brief Get current shutdown state.
     * @return Current state
     */
    virtual ShutdownState state() const = 0;

    /**
     * @brief Check if shutdown is in progress.
     * @return true if shutdown has been initiated
     */
    virtual bool isShuttingDown() const = 0;

    /**
     * @brief Check if shutdown is complete.
     * @return true if shutdown is complete
     */
    virtual bool isShutdownComplete() const = 0;

    /**
     * @brief Check if new connections can be accepted (Requirement 19.1).
     * @return true if connections can be accepted
     */
    virtual bool canAcceptConnection() const = 0;

    // -------------------------------------------------------------------------
    // State Transitions
    // -------------------------------------------------------------------------

    /**
     * @brief Called when publisher notification is complete.
     */
    virtual void notifyPublishersComplete() = 0;

    /**
     * @brief Called when grace period is complete.
     */
    virtual void gracePeriodComplete() = 0;

    /**
     * @brief Called when force termination is complete.
     */
    virtual void forceTerminationComplete() = 0;

    /**
     * @brief Called when shutdown is complete.
     */
    virtual void shutdownComplete() = 0;

    // -------------------------------------------------------------------------
    // Publisher Management (Requirement 19.2)
    // -------------------------------------------------------------------------

    /**
     * @brief Register a publisher for shutdown notification.
     * @param publisherId The publisher ID
     */
    virtual void registerPublisher(PublisherId publisherId) = 0;

    /**
     * @brief Unregister a publisher.
     * @param publisherId The publisher ID
     */
    virtual void unregisterPublisher(PublisherId publisherId) = 0;

    /**
     * @brief Get the number of registered publishers.
     * @return Publisher count
     */
    virtual size_t publisherCount() const = 0;

    // -------------------------------------------------------------------------
    // Connection Management
    // -------------------------------------------------------------------------

    /**
     * @brief Register a connection.
     * @param connectionId The connection ID
     */
    virtual void registerConnection(ConnectionId connectionId) = 0;

    /**
     * @brief Unregister a connection.
     * @param connectionId The connection ID
     */
    virtual void unregisterConnection(ConnectionId connectionId) = 0;

    /**
     * @brief Get the number of registered connections.
     * @return Connection count
     */
    virtual size_t connectionCount() const = 0;

    // -------------------------------------------------------------------------
    // Active Stream Management (Requirement 19.3)
    // -------------------------------------------------------------------------

    /**
     * @brief Register an active stream.
     * @param streamKey The stream key
     */
    virtual void registerActiveStream(const std::string& streamKey) = 0;

    /**
     * @brief Unregister an active stream.
     * @param streamKey The stream key
     */
    virtual void unregisterActiveStream(const std::string& streamKey) = 0;

    /**
     * @brief Get the number of active streams.
     * @return Active stream count
     */
    virtual size_t activeStreamCount() const = 0;

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    /**
     * @brief Get the grace period in milliseconds.
     * @return Grace period in milliseconds
     */
    virtual uint32_t gracePeriodMs() const = 0;

    /**
     * @brief Set the grace period in milliseconds.
     * @param ms Grace period in milliseconds
     */
    virtual void setGracePeriodMs(uint32_t ms) = 0;

    // -------------------------------------------------------------------------
    // Statistics (Requirement 19.6)
    // -------------------------------------------------------------------------

    /**
     * @brief Record a connection termination.
     * @param graceful Whether the termination was graceful
     */
    virtual void recordConnectionTermination(bool graceful) = 0;

    /**
     * @brief Record a stream interruption.
     */
    virtual void recordStreamInterruption() = 0;

    /**
     * @brief Get shutdown statistics.
     * @return Shutdown statistics
     */
    virtual ShutdownStatistics getShutdownStatistics() const = 0;

    /**
     * @brief Get a human-readable shutdown summary.
     * @return Summary string for logging
     */
    virtual std::string getShutdownSummary() const = 0;

    // -------------------------------------------------------------------------
    // Callbacks
    // -------------------------------------------------------------------------

    /**
     * @brief Set callback for publisher notification.
     * @param callback The callback
     */
    virtual void setPublisherNotificationCallback(PublisherNotificationCallback callback) = 0;

    /**
     * @brief Set callback for grace period expiration.
     * @param callback The callback
     */
    virtual void setGracePeriodExpiredCallback(GracePeriodExpiredCallback callback) = 0;

    /**
     * @brief Set callback for all streams ended.
     * @param callback The callback
     */
    virtual void setAllStreamsEndedCallback(AllStreamsEndedCallback callback) = 0;

    /**
     * @brief Set callback for force shutdown.
     * @param callback The callback
     */
    virtual void setForceShutdownCallback(ForceShutdownCallback callback) = 0;

    /**
     * @brief Set callback for connection force termination.
     * @param callback The callback
     */
    virtual void setConnectionForceTerminateCallback(ConnectionForceTerminateCallback callback) = 0;
};

// =============================================================================
// Shutdown Coordinator Implementation
// =============================================================================

/**
 * @brief Thread-safe shutdown coordinator implementation.
 *
 * Implements IShutdownCoordinator with:
 * - State machine for shutdown phases
 * - Publisher notification system
 * - Grace period timer
 * - Force shutdown capability
 * - Statistics tracking and logging
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses shared_mutex for read/write operations
 * - Atomic state for fast state checks
 */
class ShutdownCoordinator : public IShutdownCoordinator {
public:
    /**
     * @brief Construct a new ShutdownCoordinator.
     */
    ShutdownCoordinator();

    /**
     * @brief Destructor.
     */
    ~ShutdownCoordinator() override;

    // Non-copyable
    ShutdownCoordinator(const ShutdownCoordinator&) = delete;
    ShutdownCoordinator& operator=(const ShutdownCoordinator&) = delete;

    // Movable
    ShutdownCoordinator(ShutdownCoordinator&&) noexcept;
    ShutdownCoordinator& operator=(ShutdownCoordinator&&) noexcept;

    // IShutdownCoordinator interface implementation
    Result<void, ShutdownError> initiateShutdown() override;
    Result<void, ShutdownError> forceShutdown() override;
    void startGracePeriod() override;
    void terminateAllConnections() override;

    ShutdownState state() const override;
    bool isShuttingDown() const override;
    bool isShutdownComplete() const override;
    bool canAcceptConnection() const override;

    void notifyPublishersComplete() override;
    void gracePeriodComplete() override;
    void forceTerminationComplete() override;
    void shutdownComplete() override;

    void registerPublisher(PublisherId publisherId) override;
    void unregisterPublisher(PublisherId publisherId) override;
    size_t publisherCount() const override;

    void registerConnection(ConnectionId connectionId) override;
    void unregisterConnection(ConnectionId connectionId) override;
    size_t connectionCount() const override;

    void registerActiveStream(const std::string& streamKey) override;
    void unregisterActiveStream(const std::string& streamKey) override;
    size_t activeStreamCount() const override;

    uint32_t gracePeriodMs() const override;
    void setGracePeriodMs(uint32_t ms) override;

    void recordConnectionTermination(bool graceful) override;
    void recordStreamInterruption() override;
    ShutdownStatistics getShutdownStatistics() const override;
    std::string getShutdownSummary() const override;

    void setPublisherNotificationCallback(PublisherNotificationCallback callback) override;
    void setGracePeriodExpiredCallback(GracePeriodExpiredCallback callback) override;
    void setAllStreamsEndedCallback(AllStreamsEndedCallback callback) override;
    void setForceShutdownCallback(ForceShutdownCallback callback) override;
    void setConnectionForceTerminateCallback(ConnectionForceTerminateCallback callback) override;

private:
    // -------------------------------------------------------------------------
    // Internal Methods
    // -------------------------------------------------------------------------

    /**
     * @brief Notify all registered publishers of shutdown.
     */
    void notifyAllPublishers();

    /**
     * @brief Check if all active streams have ended.
     */
    void checkAllStreamsEnded();

    /**
     * @brief Grace period timer thread function.
     */
    void gracePeriodTimerThread();

    /**
     * @brief Stop the grace period timer.
     */
    void stopGracePeriodTimer();

    /**
     * @brief Safely invoke a callback, catching exceptions.
     */
    template<typename Callback, typename... Args>
    void safeInvokeCallback(const Callback& callback, Args&&... args);

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    std::atomic<ShutdownState> state_;

    // Publishers
    std::set<PublisherId> publishers_;

    // Connections
    std::set<ConnectionId> connections_;

    // Active streams
    std::unordered_set<std::string> activeStreams_;

    // Grace period
    uint32_t gracePeriodMs_;
    std::atomic<bool> gracePeriodTimerRunning_;
    std::thread gracePeriodThread_;
    std::condition_variable gracePeriodCv_;
    std::mutex gracePeriodMutex_;

    // Statistics
    ShutdownStatistics stats_;
    std::chrono::steady_clock::time_point shutdownStartTime_;

    // Thread safety
    mutable std::shared_mutex mutex_;
    mutable std::mutex callbackMutex_;

    // Callbacks
    PublisherNotificationCallback publisherNotificationCallback_;
    GracePeriodExpiredCallback gracePeriodExpiredCallback_;
    AllStreamsEndedCallback allStreamsEndedCallback_;
    ForceShutdownCallback forceShutdownCallback_;
    ConnectionForceTerminateCallback connectionForceTerminateCallback_;
};

} // namespace core
} // namespace openrtmp

#endif // OPENRTMP_CORE_SHUTDOWN_COORDINATOR_HPP
