// OpenRTMP - Cross-platform RTMP Server
// Server Lifecycle Manager - Integrates all components into server lifecycle
//
// Responsibilities:
// - Wire configuration loading during initialization
// - Start event loop and connection acceptance on server start
// - Coordinate component shutdown in correct order
// - Validate configuration and report errors on startup
// - Emit server lifecycle events to registered callbacks
//
// Requirements coverage:
// - Requirement 17.4: Validate configuration on startup
// - Requirement 17.5: Report errors for invalid settings

#ifndef OPENRTMP_API_SERVER_LIFECYCLE_HPP
#define OPENRTMP_API_SERVER_LIFECYCLE_HPP

#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <chrono>

#include "openrtmp/core/result.hpp"

// Forward declarations
namespace openrtmp {
namespace core {
    class ConfigManager;
    class ConnectionPool;
    class AuthService;
    class TLSService;
    class ShutdownCoordinator;
    class ErrorIsolation;
    class MetricsCollector;
    class StructuredLogger;
}
namespace streaming {
    class StreamRegistry;
}
}

namespace openrtmp {
namespace api {

// =============================================================================
// Lifecycle State Enumeration
// =============================================================================

/**
 * @brief Server lifecycle state.
 */
enum class LifecycleState {
    Uninitialized,  ///< Server has not been initialized
    Initialized,    ///< Server is initialized but not running
    Starting,       ///< Server is starting up
    Running,        ///< Server is running and accepting connections
    Stopping,       ///< Server is shutting down
    Stopped         ///< Server has stopped
};

// =============================================================================
// Lifecycle Event Types
// =============================================================================

/**
 * @brief Types of lifecycle events that can be emitted.
 */
enum class LifecycleEventType {
    ServerInitialized,      ///< Server initialization completed
    ServerStarted,          ///< Server started accepting connections
    ServerStopping,         ///< Server is beginning shutdown
    ServerStopped,          ///< Server has fully stopped
    ConfigurationError,     ///< Configuration validation error
    ComponentError,         ///< Component initialization/runtime error
    ComponentInitialized,   ///< A component was initialized
    ComponentShutdown       ///< A component was shut down
};

// =============================================================================
// Lifecycle Event Structure
// =============================================================================

/**
 * @brief Lifecycle event data.
 */
struct LifecycleEvent {
    LifecycleEventType type;            ///< Event type
    std::string message;                ///< Human-readable message
    std::string componentName;          ///< Component name (for component events)
    std::chrono::steady_clock::time_point timestamp;  ///< Event timestamp

    LifecycleEvent()
        : type(LifecycleEventType::ServerInitialized)
        , timestamp(std::chrono::steady_clock::now())
    {}

    LifecycleEvent(LifecycleEventType t, std::string msg = "",
                   std::string component = "")
        : type(t)
        , message(std::move(msg))
        , componentName(std::move(component))
        , timestamp(std::chrono::steady_clock::now())
    {}
};

// =============================================================================
// Lifecycle Error
// =============================================================================

/**
 * @brief Error codes for lifecycle operations.
 */
struct LifecycleError {
    enum class Code {
        None = 0,
        InvalidState,               ///< Operation not valid in current state
        ConfigurationInvalid,       ///< Configuration validation failed
        ConfigurationLoadFailed,    ///< Failed to load configuration
        ComponentInitFailed,        ///< Component initialization failed
        TLSInitializationFailed,    ///< TLS service initialization failed
        EventLoopFailed,            ///< Event loop failed to start
        ShutdownFailed,             ///< Shutdown operation failed
        Unknown                     ///< Unknown error
    };

    Code code = Code::None;
    std::string message;

    LifecycleError() = default;
    LifecycleError(Code c, std::string msg = "")
        : code(c), message(std::move(msg)) {}

    [[nodiscard]] bool isSuccess() const { return code == Code::None; }
};

// =============================================================================
// Lifecycle Configuration
// =============================================================================

/**
 * @brief Configuration for server lifecycle.
 */
struct LifecycleConfig {
    // Network configuration
    uint16_t port = 1935;               ///< RTMP port
    std::string bindAddress = "0.0.0.0"; ///< Bind address
    uint32_t maxConnections = 0;        ///< Max connections (0 = platform default)

    // TLS configuration
    bool enableTLS = false;             ///< Enable RTMPS
    uint16_t tlsPort = 1936;            ///< RTMPS port
    std::string tlsCertPath;            ///< TLS certificate path
    std::string tlsKeyPath;             ///< TLS private key path

    // Shutdown configuration
    uint32_t gracePeriodMs = 30000;     ///< Grace period for graceful shutdown (ms)

    // Component configuration
    bool enableMetrics = true;          ///< Enable metrics collection
    bool enableAuth = true;             ///< Enable authentication
    bool enableStreaming = true;        ///< Enable streaming features

    // Logging
    std::string logLevel = "info";      ///< Log level
    std::string logPath;                ///< Log file path (empty = stdout)
};

// =============================================================================
// Health Status
// =============================================================================

/**
 * @brief Health status for lifecycle manager.
 */
struct LifecycleHealthStatus {
    bool healthy = false;               ///< Overall health
    std::string details;                ///< Health details
    LifecycleState state = LifecycleState::Uninitialized;
    std::chrono::milliseconds uptime{0};
};

// =============================================================================
// Callback Types
// =============================================================================

/**
 * @brief Event callback function type.
 */
using LifecycleEventCallback = std::function<void(const LifecycleEvent&)>;

/**
 * @brief Component shutdown callback function type.
 */
using ComponentShutdownCallback = std::function<void(const std::string& componentName)>;

/**
 * @brief Publisher notification callback function type.
 */
using PublisherNotificationCallback = std::function<void()>;

// =============================================================================
// Server Lifecycle Manager Interface
// =============================================================================

/**
 * @brief Interface for server lifecycle management.
 */
class IServerLifecycleManager {
public:
    virtual ~IServerLifecycleManager() = default;

    // -------------------------------------------------------------------------
    // Lifecycle Operations
    // -------------------------------------------------------------------------

    /**
     * @brief Initialize the server with default configuration.
     * @return Success or error result
     */
    virtual core::Result<void, LifecycleError> initialize() = 0;

    /**
     * @brief Initialize the server with custom configuration.
     * @param config Lifecycle configuration
     * @return Success or error result
     */
    virtual core::Result<void, LifecycleError> initialize(const LifecycleConfig& config) = 0;

    /**
     * @brief Start the server.
     * @return Success or error result
     */
    virtual core::Result<void, LifecycleError> start() = 0;

    /**
     * @brief Shutdown the server.
     * @param graceful If true, perform graceful shutdown
     * @return Success or error result
     */
    virtual core::Result<void, LifecycleError> shutdown(bool graceful) = 0;

    /**
     * @brief Force immediate shutdown.
     * @return Success or error result
     */
    virtual core::Result<void, LifecycleError> forceShutdown() = 0;

    // -------------------------------------------------------------------------
    // State Accessors
    // -------------------------------------------------------------------------

    /**
     * @brief Get current lifecycle state.
     * @return Current state
     */
    virtual LifecycleState state() const = 0;

    /**
     * @brief Check if server can accept connections.
     * @return true if accepting connections
     */
    virtual bool canAcceptConnections() const = 0;

    /**
     * @brief Get effective configuration.
     * @return Current effective configuration
     */
    virtual LifecycleConfig getEffectiveConfig() const = 0;

    /**
     * @brief Get health status.
     * @return Health status
     */
    virtual LifecycleHealthStatus getHealthStatus() const = 0;

    // -------------------------------------------------------------------------
    // Component Access
    // -------------------------------------------------------------------------

    /**
     * @brief Check if a component is initialized.
     * @param componentName Component name
     * @return true if initialized
     */
    virtual bool isComponentInitialized(const std::string& componentName) const = 0;

    /**
     * @brief Get config manager.
     * @return Shared pointer to config manager
     */
    virtual std::shared_ptr<core::ConfigManager> getConfigManager() const = 0;

    /**
     * @brief Get connection pool.
     * @return Shared pointer to connection pool
     */
    virtual std::shared_ptr<core::ConnectionPool> getConnectionPool() const = 0;

    /**
     * @brief Get stream registry.
     * @return Shared pointer to stream registry
     */
    virtual std::shared_ptr<streaming::StreamRegistry> getStreamRegistry() const = 0;

    /**
     * @brief Get metrics collector.
     * @return Shared pointer to metrics collector
     */
    virtual std::shared_ptr<core::MetricsCollector> getMetricsCollector() const = 0;

    /**
     * @brief Get shutdown coordinator.
     * @return Shared pointer to shutdown coordinator
     */
    virtual std::shared_ptr<core::ShutdownCoordinator> getShutdownCoordinator() const = 0;

    /**
     * @brief Get error isolation.
     * @return Shared pointer to error isolation
     */
    virtual std::shared_ptr<core::ErrorIsolation> getErrorIsolation() const = 0;

    // -------------------------------------------------------------------------
    // Event Callbacks
    // -------------------------------------------------------------------------

    /**
     * @brief Set event callback.
     * @param callback Event callback function
     */
    virtual void setEventCallback(LifecycleEventCallback callback) = 0;

    /**
     * @brief Set component shutdown callback.
     * @param callback Shutdown callback function
     */
    virtual void setComponentShutdownCallback(ComponentShutdownCallback callback) = 0;

    /**
     * @brief Set publisher notification callback.
     * @param callback Notification callback function
     */
    virtual void setPublisherNotificationCallback(PublisherNotificationCallback callback) = 0;

    /**
     * @brief Report a component error.
     * @param componentName Component name
     * @param message Error message
     */
    virtual void reportComponentError(const std::string& componentName,
                                      const std::string& message) = 0;
};

// =============================================================================
// Server Lifecycle Manager Implementation
// =============================================================================

/**
 * @brief Thread-safe server lifecycle manager implementation.
 *
 * Manages the complete lifecycle of the RTMP server including:
 * - Configuration loading and validation
 * - Component initialization and wiring
 * - Event loop management
 * - Coordinated shutdown
 * - Event emission
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses mutex for state protection
 * - Callbacks are invoked asynchronously when possible
 */
class ServerLifecycleManager : public IServerLifecycleManager {
public:
    /**
     * @brief Default constructor.
     */
    ServerLifecycleManager();

    /**
     * @brief Construct with configuration.
     * @param config Initial configuration
     */
    explicit ServerLifecycleManager(const LifecycleConfig& config);

    /**
     * @brief Destructor.
     */
    ~ServerLifecycleManager() override;

    // Non-copyable
    ServerLifecycleManager(const ServerLifecycleManager&) = delete;
    ServerLifecycleManager& operator=(const ServerLifecycleManager&) = delete;

    // Movable
    ServerLifecycleManager(ServerLifecycleManager&&) noexcept;
    ServerLifecycleManager& operator=(ServerLifecycleManager&&) noexcept;

    // -------------------------------------------------------------------------
    // IServerLifecycleManager Implementation
    // -------------------------------------------------------------------------

    core::Result<void, LifecycleError> initialize() override;
    core::Result<void, LifecycleError> initialize(const LifecycleConfig& config) override;
    core::Result<void, LifecycleError> start() override;
    core::Result<void, LifecycleError> shutdown(bool graceful) override;
    core::Result<void, LifecycleError> forceShutdown() override;

    LifecycleState state() const override;
    bool canAcceptConnections() const override;
    LifecycleConfig getEffectiveConfig() const override;
    LifecycleHealthStatus getHealthStatus() const override;

    bool isComponentInitialized(const std::string& componentName) const override;
    std::shared_ptr<core::ConfigManager> getConfigManager() const override;
    std::shared_ptr<core::ConnectionPool> getConnectionPool() const override;
    std::shared_ptr<streaming::StreamRegistry> getStreamRegistry() const override;
    std::shared_ptr<core::MetricsCollector> getMetricsCollector() const override;
    std::shared_ptr<core::ShutdownCoordinator> getShutdownCoordinator() const override;
    std::shared_ptr<core::ErrorIsolation> getErrorIsolation() const override;

    void setEventCallback(LifecycleEventCallback callback) override;
    void setComponentShutdownCallback(ComponentShutdownCallback callback) override;
    void setPublisherNotificationCallback(PublisherNotificationCallback callback) override;
    void reportComponentError(const std::string& componentName,
                              const std::string& message) override;

private:
    // -------------------------------------------------------------------------
    // Internal Methods
    // -------------------------------------------------------------------------

    /**
     * @brief Validate configuration.
     * @param config Configuration to validate
     * @return Success or error
     */
    core::Result<void, LifecycleError> validateConfig(const LifecycleConfig& config);

    /**
     * @brief Apply platform defaults to configuration.
     * @param config Configuration to modify
     */
    void applyPlatformDefaults(LifecycleConfig& config);

    /**
     * @brief Initialize all components.
     * @return Success or error
     */
    core::Result<void, LifecycleError> initializeComponents();

    /**
     * @brief Start all components.
     * @return Success or error
     */
    core::Result<void, LifecycleError> startComponents();

    /**
     * @brief Shutdown all components in correct order.
     * @param graceful Whether to perform graceful shutdown
     */
    void shutdownComponents(bool graceful);

    /**
     * @brief Emit a lifecycle event.
     * @param event Event to emit
     */
    void emitEvent(const LifecycleEvent& event);

    /**
     * @brief Set component as initialized.
     * @param componentName Component name
     */
    void setComponentInitialized(const std::string& componentName);

    /**
     * @brief Set component as shutdown.
     * @param componentName Component name
     */
    void setComponentShutdown(const std::string& componentName);

    // -------------------------------------------------------------------------
    // Member Variables
    // -------------------------------------------------------------------------

    // State
    mutable std::shared_mutex stateMutex_;
    std::atomic<LifecycleState> state_{LifecycleState::Uninitialized};
    std::atomic<bool> acceptingConnections_{false};

    // Configuration
    mutable std::mutex configMutex_;
    LifecycleConfig config_;
    std::chrono::steady_clock::time_point startTime_;

    // Components
    mutable std::mutex componentsMutex_;
    std::shared_ptr<core::ConfigManager> configManager_;
    std::shared_ptr<core::ConnectionPool> connectionPool_;
    std::shared_ptr<streaming::StreamRegistry> streamRegistry_;
    std::shared_ptr<core::AuthService> authService_;
    std::shared_ptr<core::TLSService> tlsService_;
    std::shared_ptr<core::ShutdownCoordinator> shutdownCoordinator_;
    std::shared_ptr<core::ErrorIsolation> errorIsolation_;
    std::shared_ptr<core::MetricsCollector> metricsCollector_;
    std::shared_ptr<core::StructuredLogger> logger_;

    // Initialized components tracking
    mutable std::mutex initializedMutex_;
    std::vector<std::string> initializedComponents_;

    // Callbacks
    mutable std::mutex callbackMutex_;
    LifecycleEventCallback eventCallback_;
    ComponentShutdownCallback componentShutdownCallback_;
    PublisherNotificationCallback publisherNotificationCallback_;
};

// =============================================================================
// Utility Functions
// =============================================================================

/**
 * @brief Convert lifecycle state to string.
 * @param state The state
 * @return String representation
 */
inline const char* lifecycleStateToString(LifecycleState state) {
    switch (state) {
        case LifecycleState::Uninitialized: return "Uninitialized";
        case LifecycleState::Initialized:   return "Initialized";
        case LifecycleState::Starting:      return "Starting";
        case LifecycleState::Running:       return "Running";
        case LifecycleState::Stopping:      return "Stopping";
        case LifecycleState::Stopped:       return "Stopped";
        default:                            return "Unknown";
    }
}

/**
 * @brief Convert lifecycle event type to string.
 * @param type The event type
 * @return String representation
 */
inline const char* lifecycleEventTypeToString(LifecycleEventType type) {
    switch (type) {
        case LifecycleEventType::ServerInitialized:    return "ServerInitialized";
        case LifecycleEventType::ServerStarted:        return "ServerStarted";
        case LifecycleEventType::ServerStopping:       return "ServerStopping";
        case LifecycleEventType::ServerStopped:        return "ServerStopped";
        case LifecycleEventType::ConfigurationError:   return "ConfigurationError";
        case LifecycleEventType::ComponentError:       return "ComponentError";
        case LifecycleEventType::ComponentInitialized: return "ComponentInitialized";
        case LifecycleEventType::ComponentShutdown:    return "ComponentShutdown";
        default:                                       return "Unknown";
    }
}

/**
 * @brief Convert lifecycle error code to string.
 * @param code The error code
 * @return String representation
 */
inline const char* lifecycleErrorCodeToString(LifecycleError::Code code) {
    switch (code) {
        case LifecycleError::Code::None:                    return "None";
        case LifecycleError::Code::InvalidState:            return "InvalidState";
        case LifecycleError::Code::ConfigurationInvalid:    return "ConfigurationInvalid";
        case LifecycleError::Code::ConfigurationLoadFailed: return "ConfigurationLoadFailed";
        case LifecycleError::Code::ComponentInitFailed:     return "ComponentInitFailed";
        case LifecycleError::Code::TLSInitializationFailed: return "TLSInitializationFailed";
        case LifecycleError::Code::EventLoopFailed:         return "EventLoopFailed";
        case LifecycleError::Code::ShutdownFailed:          return "ShutdownFailed";
        case LifecycleError::Code::Unknown:                 return "Unknown";
        default:                                            return "Unknown";
    }
}

} // namespace api
} // namespace openrtmp

#endif // OPENRTMP_API_SERVER_LIFECYCLE_HPP
