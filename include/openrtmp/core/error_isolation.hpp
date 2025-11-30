// OpenRTMP - Cross-platform RTMP Server
// Error Isolation and Recovery - Fault tolerance and error handling
//
// Responsibilities:
// - Isolate single connection failures from affecting other clients
// - Reject new connections when memory allocation fails
// - Attempt component restart on thread/task crash
// - Implement circuit breaker for external service failures
// - Log diagnostic information before unrecoverable error termination
// - Provide health check API for external monitoring
//
// Requirements coverage:
// - Requirement 20.1: Single connection failure isolation
// - Requirement 20.2: Memory allocation failure handling
// - Requirement 20.3: Thread/task crash recovery
// - Requirement 20.4: Circuit breaker for external service failures
// - Requirement 20.5: Diagnostic logging before unrecoverable error termination
// - Requirement 20.6: Health check API for external monitoring

#ifndef OPENRTMP_CORE_ERROR_ISOLATION_HPP
#define OPENRTMP_CORE_ERROR_ISOLATION_HPP

#include <cstdint>
#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <thread>
#include <condition_variable>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/error_codes.hpp"
#include "openrtmp/core/result.hpp"

namespace openrtmp {
namespace core {

// =============================================================================
// Constants
// =============================================================================

namespace error_isolation {
    /// Default memory threshold percentage (90%)
    constexpr uint32_t DEFAULT_MEMORY_THRESHOLD_PERCENT = 90;

    /// Default component restart max attempts
    constexpr uint32_t DEFAULT_COMPONENT_RESTART_MAX_ATTEMPTS = 3;

    /// Default component restart delay in milliseconds
    constexpr uint32_t DEFAULT_COMPONENT_RESTART_DELAY_MS = 1000;

    /// Default circuit breaker threshold (failures before opening)
    constexpr uint32_t DEFAULT_CIRCUIT_BREAKER_THRESHOLD = 5;

    /// Default circuit breaker reset time in milliseconds
    constexpr uint32_t DEFAULT_CIRCUIT_BREAKER_RESET_MS = 30000;
}

// =============================================================================
// Enumerations
// =============================================================================

/**
 * @brief Component state enumeration.
 */
enum class ComponentState {
    Running,        ///< Component is running normally
    Restarting,     ///< Component is being restarted
    Failed          ///< Component has failed and cannot be restarted
};

/**
 * @brief Circuit breaker state enumeration.
 */
enum class CircuitBreakerState {
    Closed,     ///< Normal operation, requests go through
    Open,       ///< Service is down, requests fail fast
    HalfOpen    ///< Testing if service recovered
};

/**
 * @brief Health state enumeration.
 */
enum class HealthState {
    Healthy,    ///< All systems operating normally
    Degraded,   ///< System operating with reduced capacity
    Unhealthy   ///< System has critical failures
};

// =============================================================================
// Configuration
// =============================================================================

/**
 * @brief Configuration for error isolation and recovery.
 */
struct ErrorIsolationConfig {
    /// Memory usage percentage threshold for rejecting new connections
    uint32_t memoryThresholdPercent = error_isolation::DEFAULT_MEMORY_THRESHOLD_PERCENT;

    /// Maximum restart attempts for crashed components
    uint32_t componentRestartMaxAttempts = error_isolation::DEFAULT_COMPONENT_RESTART_MAX_ATTEMPTS;

    /// Delay between restart attempts in milliseconds
    uint32_t componentRestartDelayMs = error_isolation::DEFAULT_COMPONENT_RESTART_DELAY_MS;

    /// Number of failures before opening circuit breaker
    uint32_t circuitBreakerThreshold = error_isolation::DEFAULT_CIRCUIT_BREAKER_THRESHOLD;

    /// Time in milliseconds before trying to close circuit breaker
    uint32_t circuitBreakerResetMs = error_isolation::DEFAULT_CIRCUIT_BREAKER_RESET_MS;
};

// =============================================================================
// Statistics
// =============================================================================

/**
 * @brief Statistics for error isolation and recovery.
 */
struct ErrorIsolationStatistics {
    uint64_t connectionFailures = 0;        ///< Total connection failures
    uint64_t memoryAllocationFailures = 0;  ///< Memory allocation failures
    uint64_t componentCrashes = 0;          ///< Component crash count
    uint64_t componentRestarts = 0;         ///< Successful component restarts
    uint64_t circuitBreakerTrips = 0;       ///< Circuit breaker trip count
    uint64_t unrecoverableErrors = 0;       ///< Unrecoverable error count
};

// =============================================================================
// Health Status
// =============================================================================

/**
 * @brief Component health status for health check API.
 */
struct ComponentHealthStatus {
    std::string name;               ///< Component name
    ComponentState state;           ///< Current state
    uint32_t restartAttempts;       ///< Number of restart attempts
};

/**
 * @brief Circuit breaker status for health check API.
 */
struct CircuitBreakerStatus {
    std::string serviceName;        ///< External service name
    CircuitBreakerState state;      ///< Current state
    uint32_t failureCount;          ///< Consecutive failure count
};

/**
 * @brief Overall health status for health check API (Requirement 20.6).
 */
struct HealthStatus {
    HealthState state = HealthState::Healthy;           ///< Overall health state
    std::string details;                                ///< Human-readable details

    uint64_t memoryUsedBytes = 0;                       ///< Current memory usage
    uint64_t memoryTotalBytes = 0;                      ///< Total memory available
    uint32_t activeConnections = 0;                     ///< Active connection count

    std::vector<ComponentHealthStatus> componentStatuses;   ///< Component statuses
    std::vector<CircuitBreakerStatus> circuitBreakerStatuses;   ///< Circuit breaker statuses
};

// =============================================================================
// Callback Types
// =============================================================================

/**
 * @brief Callback for connection error notification.
 * @param connectionId The failed connection ID
 * @param code The error code
 * @param message The error message
 */
using ConnectionErrorCallback = std::function<void(
    ConnectionId connectionId,
    ErrorCode code,
    const std::string& message
)>;

/**
 * @brief Callback for memory pressure notification.
 * @param usedBytes Current memory usage
 * @param totalBytes Total memory available
 */
using MemoryPressureCallback = std::function<void(
    uint64_t usedBytes,
    uint64_t totalBytes
)>;

/**
 * @brief Callback for diagnostic logging before termination.
 * @param diagnostics Diagnostic information string
 */
using DiagnosticLoggingCallback = std::function<void(
    const std::string& diagnostics
)>;

/**
 * @brief Component restart function.
 * @return true if restart successful, false otherwise
 */
using ComponentRestartFunction = std::function<bool()>;

// =============================================================================
// Error Isolation Interface
// =============================================================================

/**
 * @brief Interface for error isolation and recovery.
 */
class IErrorIsolation {
public:
    virtual ~IErrorIsolation() = default;

    // -------------------------------------------------------------------------
    // Connection Error Isolation (Requirement 20.1)
    // -------------------------------------------------------------------------

    /**
     * @brief Register a connection for error tracking.
     * @param connectionId The connection ID
     */
    virtual void registerConnection(ConnectionId connectionId) = 0;

    /**
     * @brief Unregister a connection.
     * @param connectionId The connection ID
     */
    virtual void unregisterConnection(ConnectionId connectionId) = 0;

    /**
     * @brief Handle a connection error.
     *
     * Isolates the failure to the single connection without affecting others.
     *
     * @param connectionId The connection ID
     * @param code The error code
     * @param message The error message
     */
    virtual void handleConnectionError(
        ConnectionId connectionId,
        ErrorCode code,
        const std::string& message
    ) = 0;

    /**
     * @brief Check if a connection has failed.
     * @param connectionId The connection ID
     * @return true if connection has failed
     */
    virtual bool isConnectionFailed(ConnectionId connectionId) const = 0;

    /**
     * @brief Cleanup a failed connection.
     * @param connectionId The connection ID
     */
    virtual void cleanupConnection(ConnectionId connectionId) = 0;

    /**
     * @brief Get total connection count.
     * @return Total registered connections
     */
    virtual size_t connectionCount() const = 0;

    /**
     * @brief Get active (non-failed) connection count.
     * @return Active connection count
     */
    virtual size_t activeConnectionCount() const = 0;

    /**
     * @brief Set connection error callback.
     * @param callback The callback function
     */
    virtual void setConnectionErrorCallback(ConnectionErrorCallback callback) = 0;

    // -------------------------------------------------------------------------
    // Memory Pressure Handling (Requirement 20.2)
    // -------------------------------------------------------------------------

    /**
     * @brief Report current memory usage.
     * @param usedBytes Current memory usage in bytes
     * @param totalBytes Total available memory in bytes
     */
    virtual void reportMemoryUsage(uint64_t usedBytes, uint64_t totalBytes) = 0;

    /**
     * @brief Record a memory allocation failure.
     * @param component Component that failed to allocate
     * @param requestedBytes Requested allocation size
     */
    virtual void recordMemoryAllocationFailure(
        const std::string& component,
        uint64_t requestedBytes
    ) = 0;

    /**
     * @brief Check if system is under memory pressure.
     * @return true if memory usage exceeds threshold
     */
    virtual bool isUnderMemoryPressure() const = 0;

    /**
     * @brief Check if new connections can be accepted.
     * @return true if new connections are allowed
     */
    virtual bool canAcceptNewConnection() const = 0;

    /**
     * @brief Set memory pressure callback.
     * @param callback The callback function
     */
    virtual void setMemoryPressureCallback(MemoryPressureCallback callback) = 0;

    // -------------------------------------------------------------------------
    // Component Restart (Requirement 20.3)
    // -------------------------------------------------------------------------

    /**
     * @brief Register a component for crash recovery.
     * @param name Component name
     * @param restartFunc Function to restart the component
     */
    virtual void registerComponent(
        const std::string& name,
        ComponentRestartFunction restartFunc
    ) = 0;

    /**
     * @brief Unregister a component.
     * @param name Component name
     */
    virtual void unregisterComponent(const std::string& name) = 0;

    /**
     * @brief Report a component crash.
     *
     * Triggers restart attempt based on configuration.
     *
     * @param name Component name
     * @param errorDetails Error details for logging
     */
    virtual void reportComponentCrash(
        const std::string& name,
        const std::string& errorDetails
    ) = 0;

    /**
     * @brief Get component state.
     * @param name Component name
     * @return Current component state
     */
    virtual ComponentState getComponentState(const std::string& name) const = 0;

    // -------------------------------------------------------------------------
    // Circuit Breaker (Requirement 20.4)
    // -------------------------------------------------------------------------

    /**
     * @brief Register an external service for circuit breaker tracking.
     * @param serviceName Service name
     */
    virtual void registerExternalService(const std::string& serviceName) = 0;

    /**
     * @brief Unregister an external service.
     * @param serviceName Service name
     */
    virtual void unregisterExternalService(const std::string& serviceName) = 0;

    /**
     * @brief Record an external service failure.
     * @param serviceName Service name
     */
    virtual void recordExternalServiceFailure(const std::string& serviceName) = 0;

    /**
     * @brief Record an external service success.
     * @param serviceName Service name
     */
    virtual void recordExternalServiceSuccess(const std::string& serviceName) = 0;

    /**
     * @brief Check if an external service is available.
     *
     * May transition circuit breaker state.
     *
     * @param serviceName Service name
     * @return true if service is available
     */
    virtual bool isExternalServiceAvailable(const std::string& serviceName) = 0;

    /**
     * @brief Get circuit breaker state for a service.
     * @param serviceName Service name
     * @return Current circuit breaker state
     */
    virtual CircuitBreakerState getCircuitBreakerState(
        const std::string& serviceName
    ) const = 0;

    // -------------------------------------------------------------------------
    // Diagnostic Logging (Requirement 20.5)
    // -------------------------------------------------------------------------

    /**
     * @brief Report an unrecoverable error.
     *
     * Logs comprehensive diagnostic information before termination.
     *
     * @param errorMessage Error message
     * @param errorDetails Additional error details
     */
    virtual void reportUnrecoverableError(
        const std::string& errorMessage,
        const std::string& errorDetails
    ) = 0;

    /**
     * @brief Get diagnostic dump string.
     * @return Diagnostic information
     */
    virtual std::string getDiagnosticDump() const = 0;

    /**
     * @brief Set diagnostic logging callback.
     * @param callback The callback function
     */
    virtual void setDiagnosticLoggingCallback(DiagnosticLoggingCallback callback) = 0;

    // -------------------------------------------------------------------------
    // Health Check API (Requirement 20.6)
    // -------------------------------------------------------------------------

    /**
     * @brief Get overall health status.
     * @return Health status with component and circuit breaker information
     */
    virtual HealthStatus getHealthStatus() const = 0;

    // -------------------------------------------------------------------------
    // Statistics
    // -------------------------------------------------------------------------

    /**
     * @brief Get error isolation statistics.
     * @return Current statistics
     */
    virtual ErrorIsolationStatistics getStatistics() const = 0;

    /**
     * @brief Reset statistics to zero.
     */
    virtual void resetStatistics() = 0;

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    /**
     * @brief Update configuration.
     * @param updater Function to modify configuration
     */
    virtual void updateConfig(std::function<void(ErrorIsolationConfig&)> updater) = 0;

    /**
     * @brief Get current configuration.
     * @return Current configuration
     */
    virtual ErrorIsolationConfig getConfig() const = 0;
};

// =============================================================================
// Error Isolation Implementation
// =============================================================================

/**
 * @brief Thread-safe error isolation and recovery implementation.
 *
 * Implements IErrorIsolation with:
 * - Connection error isolation
 * - Memory pressure detection
 * - Component crash recovery
 * - Circuit breaker pattern
 * - Diagnostic logging
 * - Health check API
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses shared_mutex for read/write operations
 * - Uses mutex for callback invocations
 */
class ErrorIsolation : public IErrorIsolation {
public:
    /**
     * @brief Construct with default configuration.
     */
    ErrorIsolation();

    /**
     * @brief Construct with configuration.
     * @param config Error isolation configuration
     */
    explicit ErrorIsolation(const ErrorIsolationConfig& config);

    /**
     * @brief Destructor.
     */
    ~ErrorIsolation() override;

    // Non-copyable
    ErrorIsolation(const ErrorIsolation&) = delete;
    ErrorIsolation& operator=(const ErrorIsolation&) = delete;

    // Movable
    ErrorIsolation(ErrorIsolation&&) noexcept;
    ErrorIsolation& operator=(ErrorIsolation&&) noexcept;

    // IErrorIsolation interface implementation
    void registerConnection(ConnectionId connectionId) override;
    void unregisterConnection(ConnectionId connectionId) override;
    void handleConnectionError(
        ConnectionId connectionId,
        ErrorCode code,
        const std::string& message
    ) override;
    bool isConnectionFailed(ConnectionId connectionId) const override;
    void cleanupConnection(ConnectionId connectionId) override;
    size_t connectionCount() const override;
    size_t activeConnectionCount() const override;
    void setConnectionErrorCallback(ConnectionErrorCallback callback) override;

    void reportMemoryUsage(uint64_t usedBytes, uint64_t totalBytes) override;
    void recordMemoryAllocationFailure(
        const std::string& component,
        uint64_t requestedBytes
    ) override;
    bool isUnderMemoryPressure() const override;
    bool canAcceptNewConnection() const override;
    void setMemoryPressureCallback(MemoryPressureCallback callback) override;

    void registerComponent(
        const std::string& name,
        ComponentRestartFunction restartFunc
    ) override;
    void unregisterComponent(const std::string& name) override;
    void reportComponentCrash(
        const std::string& name,
        const std::string& errorDetails
    ) override;
    ComponentState getComponentState(const std::string& name) const override;

    void registerExternalService(const std::string& serviceName) override;
    void unregisterExternalService(const std::string& serviceName) override;
    void recordExternalServiceFailure(const std::string& serviceName) override;
    void recordExternalServiceSuccess(const std::string& serviceName) override;
    bool isExternalServiceAvailable(const std::string& serviceName) override;
    CircuitBreakerState getCircuitBreakerState(
        const std::string& serviceName
    ) const override;

    void reportUnrecoverableError(
        const std::string& errorMessage,
        const std::string& errorDetails
    ) override;
    std::string getDiagnosticDump() const override;
    void setDiagnosticLoggingCallback(DiagnosticLoggingCallback callback) override;

    HealthStatus getHealthStatus() const override;

    ErrorIsolationStatistics getStatistics() const override;
    void resetStatistics() override;

    void updateConfig(std::function<void(ErrorIsolationConfig&)> updater) override;
    ErrorIsolationConfig getConfig() const override;

private:
    // -------------------------------------------------------------------------
    // Internal Types
    // -------------------------------------------------------------------------

    struct ConnectionInfo {
        ConnectionId id;
        bool failed = false;
        std::string lastError;
    };

    struct ComponentInfo {
        std::string name;
        ComponentState state = ComponentState::Running;
        ComponentRestartFunction restartFunc;
        uint32_t restartAttempts = 0;
        std::thread restartThread;
    };

    struct CircuitBreakerInfo {
        std::string serviceName;
        CircuitBreakerState state = CircuitBreakerState::Closed;
        uint32_t failureCount = 0;
        std::chrono::steady_clock::time_point lastFailureTime;
    };

    // -------------------------------------------------------------------------
    // Internal Methods
    // -------------------------------------------------------------------------

    /**
     * @brief Attempt to restart a component.
     */
    void attemptComponentRestart(const std::string& name);

    /**
     * @brief Safely invoke a callback.
     */
    template<typename Callback, typename... Args>
    void safeInvokeCallback(const Callback& callback, Args&&... args);

    /**
     * @brief Calculate memory usage percentage.
     */
    uint32_t calculateMemoryPercent() const;

    /**
     * @brief Build diagnostic string.
     */
    std::string buildDiagnostics() const;

    // -------------------------------------------------------------------------
    // Member Variables
    // -------------------------------------------------------------------------

    // Configuration
    ErrorIsolationConfig config_;
    mutable std::shared_mutex configMutex_;

    // Connections
    std::unordered_map<ConnectionId, ConnectionInfo> connections_;
    mutable std::shared_mutex connectionsMutex_;

    // Memory
    std::atomic<uint64_t> memoryUsedBytes_{0};
    std::atomic<uint64_t> memoryTotalBytes_{0};
    std::atomic<bool> underMemoryPressure_{false};

    // Components
    std::unordered_map<std::string, std::unique_ptr<ComponentInfo>> components_;
    mutable std::shared_mutex componentsMutex_;

    // Circuit breakers
    std::unordered_map<std::string, CircuitBreakerInfo> circuitBreakers_;
    mutable std::shared_mutex circuitBreakersMutex_;

    // Statistics
    ErrorIsolationStatistics stats_;
    mutable std::mutex statsMutex_;

    // Callbacks
    ConnectionErrorCallback connectionErrorCallback_;
    MemoryPressureCallback memoryPressureCallback_;
    DiagnosticLoggingCallback diagnosticLoggingCallback_;
    mutable std::mutex callbackMutex_;

    // Shutdown flag
    std::atomic<bool> shuttingDown_{false};
};

// =============================================================================
// Utility Functions
// =============================================================================

/**
 * @brief Convert component state to string.
 * @param state The component state
 * @return String representation
 */
inline const char* componentStateToString(ComponentState state) {
    switch (state) {
        case ComponentState::Running:     return "Running";
        case ComponentState::Restarting:  return "Restarting";
        case ComponentState::Failed:      return "Failed";
        default:                          return "Unknown";
    }
}

/**
 * @brief Convert circuit breaker state to string.
 * @param state The circuit breaker state
 * @return String representation
 */
inline const char* circuitBreakerStateToString(CircuitBreakerState state) {
    switch (state) {
        case CircuitBreakerState::Closed:   return "Closed";
        case CircuitBreakerState::Open:     return "Open";
        case CircuitBreakerState::HalfOpen: return "HalfOpen";
        default:                            return "Unknown";
    }
}

/**
 * @brief Convert health state to string.
 * @param state The health state
 * @return String representation
 */
inline const char* healthStateToString(HealthState state) {
    switch (state) {
        case HealthState::Healthy:   return "Healthy";
        case HealthState::Degraded:  return "Degraded";
        case HealthState::Unhealthy: return "Unhealthy";
        default:                     return "Unknown";
    }
}

} // namespace core
} // namespace openrtmp

#endif // OPENRTMP_CORE_ERROR_ISOLATION_HPP
