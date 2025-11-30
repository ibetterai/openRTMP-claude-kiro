// OpenRTMP - Cross-platform RTMP Server
// Connection Pool - Pre-allocated connection management with platform-aware limits
//
// Responsibilities:
// - Pre-allocate connection objects to avoid allocation during accept
// - Support 1000 concurrent connections on desktop platforms
// - Support 100 concurrent connections on mobile platforms
// - Reject new connections with appropriate error when limits reached
// - Log connection limit events for capacity planning
//
// Requirements coverage:
// - Requirement 14.1: Desktop platforms support 1000 concurrent TCP connections
// - Requirement 14.2: Mobile platforms support 100 concurrent TCP connections
// - Requirement 14.5: Connection pooling for efficient resource management
// - Requirement 14.6: Reject new connections with error when limits reached

#ifndef OPENRTMP_CORE_CONNECTION_POOL_HPP
#define OPENRTMP_CORE_CONNECTION_POOL_HPP

#include <cstdint>
#include <string>
#include <optional>
#include <memory>
#include <functional>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <chrono>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/result.hpp"
#include "openrtmp/core/error_codes.hpp"

namespace openrtmp {
namespace core {

// =============================================================================
// Constants
// =============================================================================

/// Invalid connection handle constant
constexpr uint64_t INVALID_CONNECTION_HANDLE = 0;

/// Desktop platform connection limit (Requirement 14.1)
constexpr size_t DESKTOP_CONNECTION_LIMIT = 1000;

/// Mobile platform connection limit (Requirement 14.2)
constexpr size_t MOBILE_CONNECTION_LIMIT = 100;

/// Default pre-allocation percentage
constexpr size_t DEFAULT_PREALLOCATE_PERCENT = 50;

/// Default high watermark percentage for logging
constexpr size_t DEFAULT_HIGH_WATERMARK_PERCENT = 80;

/// Default low watermark percentage for logging
constexpr size_t DEFAULT_LOW_WATERMARK_PERCENT = 50;

// =============================================================================
// Type Definitions
// =============================================================================

/// Connection handle type for pool operations
using ConnectionHandle = uint64_t;

// =============================================================================
// Connection Structure
// =============================================================================

/**
 * @brief Represents a connection in the pool.
 *
 * This structure holds all state for a single RTMP connection.
 * It is managed by the ConnectionPool and should be accessed
 * through the pool's methods.
 */
struct Connection {
    /// Unique connection identifier
    ConnectionId id{INVALID_CONNECTION_ID};

    /// Client IP address
    std::string clientIP;

    /// Client port
    uint16_t clientPort{0};

    /// Connection state
    ConnectionState state{ConnectionState::Connecting};

    /// Connection timestamp
    std::chrono::steady_clock::time_point connectedAt;

    /// Last activity timestamp
    std::chrono::steady_clock::time_point lastActivityAt;

    /// Associated session ID (if any)
    SessionId sessionId{INVALID_SESSION_ID};

    /// User agent string
    std::string userAgent;

    /// Is connection over TLS
    bool isTLS{false};

    /**
     * @brief Reset connection to initial state.
     *
     * Called when connection is returned to pool.
     */
    void reset() {
        id = INVALID_CONNECTION_ID;
        clientIP.clear();
        clientPort = 0;
        state = ConnectionState::Connecting;
        connectedAt = std::chrono::steady_clock::time_point{};
        lastActivityAt = std::chrono::steady_clock::time_point{};
        sessionId = INVALID_SESSION_ID;
        userAgent.clear();
        isTLS = false;
    }
};

// =============================================================================
// Configuration Types
// =============================================================================

/**
 * @brief Configuration for the connection pool.
 */
struct ConnectionPoolConfig {
    /// Maximum number of concurrent connections
    size_t maxConnections{0};  // 0 = use platform default

    /// Number of connections to pre-allocate
    size_t preAllocateCount{0};  // 0 = auto-calculate

    /// High watermark percentage for logging (default 80%)
    size_t highWatermarkPercent{DEFAULT_HIGH_WATERMARK_PERCENT};

    /// Low watermark percentage for recovery logging (default 50%)
    size_t lowWatermarkPercent{DEFAULT_LOW_WATERMARK_PERCENT};

    /// Whether to enable connection recycling
    bool enableRecycling{true};

    /**
     * @brief Create configuration with platform-appropriate defaults.
     *
     * Desktop: 1000 connections (Requirement 14.1)
     * Mobile: 100 connections (Requirement 14.2)
     *
     * @return Configuration with platform defaults
     */
    static ConnectionPoolConfig platformDefault() {
        ConnectionPoolConfig config;

        if (isDesktopPlatform()) {
            config.maxConnections = DESKTOP_CONNECTION_LIMIT;
        } else if (isMobilePlatform()) {
            config.maxConnections = MOBILE_CONNECTION_LIMIT;
        } else {
            // Fallback to conservative default
            config.maxConnections = MOBILE_CONNECTION_LIMIT;
        }

        // Pre-allocate half of max connections by default
        config.preAllocateCount = config.maxConnections * DEFAULT_PREALLOCATE_PERCENT / 100;

        return config;
    }
};

// =============================================================================
// Statistics Types
// =============================================================================

/**
 * @brief Statistics for the connection pool.
 *
 * Used for monitoring and capacity planning.
 */
struct ConnectionPoolStatistics {
    /// Number of connections currently in use
    size_t connectionsInUse{0};

    /// Number of connections available in pool
    size_t availableConnections{0};

    /// Peak number of connections in use
    size_t peakConnectionsInUse{0};

    /// Total number of allocated connection objects
    size_t totalAllocatedConnections{0};

    /// Number of connection requests rejected due to limit
    size_t rejectedConnections{0};

    /// Total number of acquire operations
    size_t totalAcquires{0};

    /// Total number of release operations
    size_t totalReleases{0};

    /// Total allocations (new connection objects created)
    size_t totalAllocations{0};

    /// Time of last statistics reset
    std::chrono::steady_clock::time_point lastResetTime;
};

// =============================================================================
// Event Types for Logging
// =============================================================================

/**
 * @brief Connection limit event type for logging callbacks.
 */
enum class ConnectionLimitEventType {
    /// Connection limit has been reached
    LimitReached,

    /// High watermark threshold crossed (going up)
    HighWatermark,

    /// Low watermark threshold crossed (going down)
    LowWatermark,

    /// Connection rejected due to limit
    ConnectionRejected
};

/**
 * @brief Connection limit event data for logging.
 */
struct ConnectionLimitEvent {
    /// Type of limit event
    ConnectionLimitEventType type;

    /// Current connection count
    size_t currentCount{0};

    /// Maximum allowed connections
    size_t maxCount{0};

    /// Event timestamp
    std::chrono::steady_clock::time_point timestamp;

    /// Optional message with additional context
    std::string message;
};

// =============================================================================
// Callback Types
// =============================================================================

/**
 * @brief Callback for connection limit events (Requirement 14.6).
 *
 * Used for logging and capacity planning.
 */
using ConnectionLimitCallback = std::function<void(const ConnectionLimitEvent&)>;

// =============================================================================
// Connection Pool Interface
// =============================================================================

/**
 * @brief Interface for connection pool operations.
 */
class IConnectionPool {
public:
    virtual ~IConnectionPool() = default;

    // -------------------------------------------------------------------------
    // Connection Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Acquire a connection from the pool.
     *
     * Returns a connection handle that can be used to access the connection.
     * If the pool limit is reached, returns an error (Requirement 14.6).
     *
     * @return Result containing handle or error
     */
    virtual Result<ConnectionHandle, Error> acquire() = 0;

    /**
     * @brief Release a connection back to the pool.
     *
     * The connection is reset and returned to the available pool.
     *
     * @param handle The connection handle to release
     */
    virtual void release(ConnectionHandle handle) = 0;

    /**
     * @brief Get a connection object by handle.
     *
     * @param handle The connection handle
     * @return Pointer to connection or nullptr if invalid
     */
    virtual Connection* getConnection(ConnectionHandle handle) = 0;

    /**
     * @brief Get a connection object by handle (const version).
     *
     * @param handle The connection handle
     * @return Const pointer to connection or nullptr if invalid
     */
    virtual const Connection* getConnection(ConnectionHandle handle) const = 0;

    // -------------------------------------------------------------------------
    // Configuration and Statistics
    // -------------------------------------------------------------------------

    /**
     * @brief Get current pool configuration.
     *
     * @return Pool configuration
     */
    virtual ConnectionPoolConfig getConfig() const = 0;

    /**
     * @brief Get pool statistics.
     *
     * @return Current statistics
     */
    virtual ConnectionPoolStatistics getStatistics() const = 0;

    /**
     * @brief Reset pool statistics.
     */
    virtual void resetStatistics() = 0;

    // -------------------------------------------------------------------------
    // Callbacks
    // -------------------------------------------------------------------------

    /**
     * @brief Set callback for connection limit events.
     *
     * Used for logging and capacity planning (Requirement 14.6).
     *
     * @param callback The callback function
     */
    virtual void setConnectionLimitCallback(ConnectionLimitCallback callback) = 0;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Clear all connections from the pool.
     *
     * All active connections are released and the pool is reset.
     */
    virtual void clear() = 0;
};

// =============================================================================
// Connection Pool Implementation
// =============================================================================

/**
 * @brief Thread-safe connection pool implementation.
 *
 * Provides:
 * - Pre-allocated connection objects (Requirement 14.5)
 * - Platform-aware connection limits (Requirements 14.1, 14.2)
 * - Rejection with error when limit reached (Requirement 14.6)
 * - Logging callbacks for capacity planning
 * - Statistics tracking for monitoring
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses std::shared_mutex for read/write locking
 */
class ConnectionPool : public IConnectionPool {
public:
    /**
     * @brief Construct a connection pool with given configuration.
     *
     * @param config Pool configuration (use platformDefault() for defaults)
     */
    explicit ConnectionPool(const ConnectionPoolConfig& config = ConnectionPoolConfig::platformDefault());

    /**
     * @brief Destructor.
     */
    ~ConnectionPool() override;

    // Non-copyable
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    // Movable
    ConnectionPool(ConnectionPool&&) noexcept;
    ConnectionPool& operator=(ConnectionPool&&) noexcept;

    // IConnectionPool interface implementation
    Result<ConnectionHandle, Error> acquire() override;
    void release(ConnectionHandle handle) override;
    Connection* getConnection(ConnectionHandle handle) override;
    const Connection* getConnection(ConnectionHandle handle) const override;
    ConnectionPoolConfig getConfig() const override;
    ConnectionPoolStatistics getStatistics() const override;
    void resetStatistics() override;
    void setConnectionLimitCallback(ConnectionLimitCallback callback) override;
    void clear() override;

private:
    /**
     * @brief Internal connection entry with metadata.
     */
    struct ConnectionEntry {
        std::unique_ptr<Connection> connection;
        ConnectionHandle handle{INVALID_CONNECTION_HANDLE};
        bool inUse{false};
    };

    /**
     * @brief Pre-allocate connection objects.
     */
    void preAllocate();

    /**
     * @brief Allocate a new connection entry.
     *
     * @return Pointer to new entry or nullptr if allocation failed
     */
    ConnectionEntry* allocateEntry();

    /**
     * @brief Generate a unique connection handle.
     *
     * @return New unique handle
     */
    ConnectionHandle generateHandle();

    /**
     * @brief Check and emit watermark events.
     *
     * @param newCount The new connection count
     */
    void checkWatermarks(size_t newCount);

    /**
     * @brief Emit a limit event.
     *
     * @param type Event type
     * @param message Optional message
     */
    void emitLimitEvent(ConnectionLimitEventType type, const std::string& message = "");

    // Configuration
    ConnectionPoolConfig config_;

    // Connection storage
    std::vector<std::unique_ptr<ConnectionEntry>> entries_;

    // Handle to entry index mapping
    std::unordered_map<ConnectionHandle, size_t> handleToIndex_;

    // Available entry indices (free pool)
    std::queue<size_t> availableIndices_;

    // Statistics
    mutable ConnectionPoolStatistics statistics_;

    // Watermark state
    bool aboveHighWatermark_{false};

    // Handle counter
    std::atomic<uint64_t> nextHandle_{1};

    // Thread safety
    mutable std::shared_mutex mutex_;

    // Callback
    ConnectionLimitCallback limitCallback_;
    std::mutex callbackMutex_;
};

} // namespace core
} // namespace openrtmp

#endif // OPENRTMP_CORE_CONNECTION_POOL_HPP
