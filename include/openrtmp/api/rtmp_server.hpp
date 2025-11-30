// OpenRTMP - Cross-platform RTMP Server
// RTMPServer Public API - Main server interface
//
// Responsibilities:
// - Expose initialize, start, and stop lifecycle methods
// - Support graceful and force stop modes
// - Provide runtime configuration update capability
// - Expose server and per-stream metrics through API
// - Support authentication and event callbacks
// - Ensure thread-safe method invocation from any thread
//
// Requirements coverage:
// - Requirement 6.1: Single executable with configurable parameters
// - Requirement 17.3: Thread-safe operation handling

#ifndef OPENRTMP_API_RTMP_SERVER_HPP
#define OPENRTMP_API_RTMP_SERVER_HPP

#include <cstdint>
#include <string>
#include <optional>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <shared_mutex>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/result.hpp"

namespace openrtmp {
namespace api {

// =============================================================================
// Server State Enumeration
// =============================================================================

/**
 * @brief Server lifecycle state enumeration.
 */
enum class ServerState {
    Uninitialized,  ///< Server has not been initialized
    Initialized,    ///< Server is initialized but not running
    Starting,       ///< Server is starting up
    Running,        ///< Server is running and accepting connections
    Stopping,       ///< Server is in graceful shutdown
    Stopped         ///< Server has stopped
};

/**
 * @brief Convert server state to string.
 * @param state The server state
 * @return String representation
 */
inline const char* serverStateToString(ServerState state) {
    switch (state) {
        case ServerState::Uninitialized: return "Uninitialized";
        case ServerState::Initialized:   return "Initialized";
        case ServerState::Starting:      return "Starting";
        case ServerState::Running:       return "Running";
        case ServerState::Stopping:      return "Stopping";
        case ServerState::Stopped:       return "Stopped";
        default:                         return "Unknown";
    }
}

// =============================================================================
// Error Types
// =============================================================================

/**
 * @brief Server error information.
 */
struct ServerError {
    /**
     * @brief Error codes for server operations.
     */
    enum class Code {
        InvalidConfiguration,   ///< Configuration is invalid
        InvalidState,           ///< Operation not allowed in current state
        BindFailed,             ///< Failed to bind to port
        StartFailed,            ///< Failed to start server
        StopFailed,             ///< Failed to stop server
        InternalError           ///< Internal error occurred
    };

    Code code;              ///< Error code
    std::string message;    ///< Human-readable error message

    ServerError(Code c = Code::InternalError, std::string msg = "")
        : code(c), message(std::move(msg)) {}
};

// =============================================================================
// Configuration Types
// =============================================================================

/**
 * @brief Server initialization configuration.
 */
struct ServerConfig {
    /// Port to listen on (default: 1935)
    uint16_t port = 1935;

    /// Address to bind to (default: "0.0.0.0" for all interfaces)
    std::string bindAddress = "0.0.0.0";

    /// Maximum concurrent connections (0 = platform default)
    uint32_t maxConnections = 0;

    /// Grace period for graceful shutdown in milliseconds (default: 30000)
    uint32_t gracePeriodMs = 30000;

    /// Enable TLS/RTMPS (default: false)
    bool enableTLS = false;

    /// TLS certificate path (required if enableTLS is true)
    std::string tlsCertPath;

    /// TLS key path (required if enableTLS is true)
    std::string tlsKeyPath;

    /// Application name (default: "live")
    std::string applicationName = "live";

    /// Server name for identification
    std::string serverName = "OpenRTMP";
};

/**
 * @brief Runtime configuration update parameters.
 *
 * Fields set to 0 or empty are ignored (no change).
 */
struct RuntimeConfigUpdate {
    /// Maximum concurrent connections (0 = no change)
    uint32_t maxConnections = 0;

    /// Grace period for graceful shutdown in milliseconds (0 = no change)
    uint32_t gracePeriodMs = 0;
};

// =============================================================================
// Metrics Types
// =============================================================================

/**
 * @brief Server-wide metrics.
 */
struct ServerMetrics {
    /// Currently active connections
    uint32_t activeConnections = 0;

    /// Total connections since server start
    uint64_t totalConnections = 0;

    /// Currently active streams
    uint32_t activeStreams = 0;

    /// Total streams since server start
    uint64_t totalStreams = 0;

    /// Total bytes received
    uint64_t bytesReceived = 0;

    /// Total bytes sent
    uint64_t bytesSent = 0;

    /// Server uptime in seconds
    uint64_t uptimeSeconds = 0;

    /// Peak concurrent connections
    uint32_t peakConnections = 0;

    /// Peak concurrent streams
    uint32_t peakStreams = 0;

    /// Total rejected connections (due to limits)
    uint64_t rejectedConnections = 0;

    /// Total authentication failures
    uint64_t authFailures = 0;
};

/**
 * @brief Per-stream metrics.
 */
struct StreamMetrics {
    /// Stream key
    std::string streamKey;

    /// Stream state
    core::StreamState state = core::StreamState::Idle;

    /// Stream duration in seconds
    uint64_t durationSeconds = 0;

    /// Bytes received for this stream
    uint64_t bytesReceived = 0;

    /// Bytes sent for this stream (to all subscribers)
    uint64_t bytesSent = 0;

    /// Current subscriber count
    uint32_t subscriberCount = 0;

    /// Total subscribers joined since stream start
    uint64_t totalSubscribers = 0;

    /// Current video bitrate in kbps
    uint32_t videoBitrateKbps = 0;

    /// Current audio bitrate in kbps
    uint32_t audioBitrateKbps = 0;

    /// Video codec
    core::VideoCodec videoCodec = core::VideoCodec::Unknown;

    /// Audio codec
    core::AudioCodec audioCodec = core::AudioCodec::Unknown;
};

// =============================================================================
// Health Status
// =============================================================================

/**
 * @brief Server health status.
 */
struct HealthStatus {
    /// Whether the server is healthy
    bool healthy = false;

    /// Human-readable details
    std::string details;

    /// Memory usage percentage
    uint32_t memoryUsagePercent = 0;

    /// CPU usage percentage
    uint32_t cpuUsagePercent = 0;

    /// Current connection utilization percentage
    uint32_t connectionUtilizationPercent = 0;
};

// =============================================================================
// Callback Types
// =============================================================================

/**
 * @brief Authentication request from client.
 */
struct AuthRequest {
    /// Stream key being requested
    std::string streamKey;

    /// Application name
    std::string app;

    /// Client IP address
    std::string clientIP;

    /// Client port
    uint16_t clientPort = 0;

    /// Is publish request (true) or subscribe request (false)
    bool isPublish = false;

    /// User-agent string
    std::string userAgent;
};

/**
 * @brief Authentication response.
 */
struct AuthResponse {
    /// Whether the request is allowed
    bool allowed = false;

    /// Reason for denial (if not allowed)
    std::string reason;
};

/**
 * @brief Authentication callback type.
 */
using AuthCallback = std::function<AuthResponse(const AuthRequest&)>;

/**
 * @brief Server event types.
 */
enum class ServerEventType {
    ServerStarted,          ///< Server has started
    ServerStopped,          ///< Server has stopped
    ClientConnected,        ///< A client has connected
    ClientDisconnected,     ///< A client has disconnected
    StreamStarted,          ///< A stream has started publishing
    StreamEnded,            ///< A stream has stopped publishing
    SubscriberJoined,       ///< A subscriber joined a stream
    SubscriberLeft,         ///< A subscriber left a stream
    AuthenticationFailed,   ///< An authentication attempt failed
    ConnectionLimitReached, ///< Connection limit was reached
    Error                   ///< An error occurred
};

/**
 * @brief Server event data.
 */
struct ServerEvent {
    /// Event type
    ServerEventType type = ServerEventType::Error;

    /// Event timestamp (milliseconds since epoch)
    uint64_t timestampMs = 0;

    /// Related stream key (if applicable)
    std::string streamKey;

    /// Related client IP (if applicable)
    std::string clientIP;

    /// Related client port (if applicable)
    uint16_t clientPort = 0;

    /// Additional message/details
    std::string message;
};

/**
 * @brief Event callback type.
 */
using EventCallback = std::function<void(const ServerEvent&)>;

// =============================================================================
// RTMPServer Interface
// =============================================================================

/**
 * @brief RTMP Server public API.
 *
 * This class provides the main interface for controlling an RTMP server.
 * All methods are thread-safe and can be called from any thread.
 *
 * Typical usage:
 * @code
 * RTMPServer server;
 *
 * ServerConfig config;
 * config.port = 1935;
 * config.maxConnections = 1000;
 *
 * if (server.initialize(config).isSuccess()) {
 *     server.setAuthCallback([](const AuthRequest& req) {
 *         return AuthResponse{true, ""};
 *     });
 *
 *     if (server.start().isSuccess()) {
 *         // Server is running...
 *         // Later:
 *         server.stop(true); // Graceful shutdown
 *     }
 * }
 * @endcode
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Callbacks may be invoked from internal threads
 * - Safe to call any method from callback context
 */
class RTMPServer {
public:
    // -------------------------------------------------------------------------
    // Construction and Destruction
    // -------------------------------------------------------------------------

    /**
     * @brief Default constructor.
     *
     * Creates an uninitialized server. Call initialize() before start().
     */
    RTMPServer();

    /**
     * @brief Constructor with configuration.
     *
     * Creates a server with the given configuration.
     * Still requires initialize() to be called before start().
     *
     * @param config Server configuration
     */
    explicit RTMPServer(const ServerConfig& config);

    /**
     * @brief Destructor.
     *
     * Automatically performs force stop if server is running.
     */
    ~RTMPServer();

    // Non-copyable
    RTMPServer(const RTMPServer&) = delete;
    RTMPServer& operator=(const RTMPServer&) = delete;

    // Movable
    RTMPServer(RTMPServer&& other) noexcept;
    RTMPServer& operator=(RTMPServer&& other) noexcept;

    // -------------------------------------------------------------------------
    // Lifecycle Methods
    // -------------------------------------------------------------------------

    /**
     * @brief Initialize the server with configuration.
     *
     * Must be called before start(). Can only be called once.
     *
     * @param config Server configuration
     * @return Result indicating success or error
     */
    core::Result<void, ServerError> initialize(const ServerConfig& config);

    /**
     * @brief Start the server.
     *
     * Begins accepting connections on the configured port.
     * Server must be initialized first.
     *
     * @return Result indicating success or error
     */
    core::Result<void, ServerError> start();

    /**
     * @brief Stop the server.
     *
     * @param graceful If true, wait for grace period and notify clients.
     *                 If false, stop immediately.
     * @return Result indicating success or error
     */
    core::Result<void, ServerError> stop(bool graceful);

    /**
     * @brief Force stop the server immediately.
     *
     * Equivalent to stop(false). Terminates all connections immediately
     * without waiting for grace period.
     *
     * @return Result indicating success or error
     */
    core::Result<void, ServerError> forceStop();

    // -------------------------------------------------------------------------
    // Runtime Configuration
    // -------------------------------------------------------------------------

    /**
     * @brief Update runtime configuration.
     *
     * Allows changing certain configuration values while server is running.
     * Server must be initialized.
     *
     * @param update Configuration update parameters
     * @return Result indicating success or error
     */
    core::Result<void, ServerError> updateConfig(const RuntimeConfigUpdate& update);

    /**
     * @brief Get current configuration.
     *
     * @return Current server configuration
     */
    ServerConfig getConfig() const;

    // -------------------------------------------------------------------------
    // State Accessors
    // -------------------------------------------------------------------------

    /**
     * @brief Get current server state.
     *
     * @return Current server state
     */
    ServerState state() const;

    /**
     * @brief Check if server is running.
     *
     * @return true if server is in Running state
     */
    bool isRunning() const;

    // -------------------------------------------------------------------------
    // Metrics
    // -------------------------------------------------------------------------

    /**
     * @brief Get server-wide metrics.
     *
     * @return Current server metrics
     */
    ServerMetrics getServerMetrics() const;

    /**
     * @brief Get metrics for a specific stream.
     *
     * @param streamKey Stream key to query
     * @return Stream metrics if stream exists, nullopt otherwise
     */
    std::optional<StreamMetrics> getStreamMetrics(const std::string& streamKey) const;

    // -------------------------------------------------------------------------
    // Health Check
    // -------------------------------------------------------------------------

    /**
     * @brief Get server health status.
     *
     * @return Health status information
     */
    HealthStatus getHealthStatus() const;

    // -------------------------------------------------------------------------
    // Callbacks
    // -------------------------------------------------------------------------

    /**
     * @brief Set authentication callback.
     *
     * The callback is invoked for each publish or subscribe request.
     * Returning AuthResponse{false, reason} will reject the request.
     *
     * @param callback Authentication callback (nullptr to clear)
     */
    void setAuthCallback(AuthCallback callback);

    /**
     * @brief Set event callback.
     *
     * The callback is invoked for various server events.
     *
     * @param callback Event callback (nullptr to clear)
     */
    void setEventCallback(EventCallback callback);

    // -------------------------------------------------------------------------
    // Static Information
    // -------------------------------------------------------------------------

    /**
     * @brief Get library version string.
     *
     * @return Version string (e.g., "0.1.0")
     */
    static std::string getVersion();

private:
    // Forward declaration of implementation
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace api

// Re-export to openrtmp namespace for convenience
using api::RTMPServer;
using api::ServerConfig;
using api::ServerState;
using api::ServerError;
using api::ServerMetrics;
using api::StreamMetrics;
using api::RuntimeConfigUpdate;
using api::AuthRequest;
using api::AuthResponse;
using api::AuthCallback;
using api::ServerEvent;
using api::ServerEventType;
using api::EventCallback;
using api::HealthStatus;

} // namespace openrtmp

#endif // OPENRTMP_API_RTMP_SERVER_HPP
