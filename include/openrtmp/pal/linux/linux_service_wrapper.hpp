// OpenRTMP - Cross-platform RTMP Server
// Linux Service Wrapper Interface
//
// This component provides Linux-specific service management functionality:
// - Systemd service unit configuration generation
// - Network interface enumeration and binding
// - Signal handling for daemon operation (SIGTERM, SIGINT, SIGHUP, SIGUSR1/2)
// - PID file management
// - Support for multiple instances via template units
// - Service status and health reporting
// - Graceful shutdown coordination
//
// Requirements Covered: 7.4, 7.5

#ifndef OPENRTMP_PAL_LINUX_SERVICE_WRAPPER_HPP
#define OPENRTMP_PAL_LINUX_SERVICE_WRAPPER_HPP

#if defined(__linux__)

#include "openrtmp/core/result.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace openrtmp {
namespace pal {
namespace linux_pal {

// =============================================================================
// Enumerations
// =============================================================================

/**
 * @brief Systemd service types.
 */
enum class SystemdServiceType {
    Simple,      ///< Default; process started by ExecStart is the main process
    Exec,        ///< Like simple, but systemd waits for the binary to be executed
    Forking,     ///< Service forks; parent exits, child continues
    Oneshot,     ///< Service is expected to exit; useful for setup tasks
    Dbus,        ///< Similar to simple, but waits for D-Bus name
    Notify,      ///< Service sends notification via sd_notify when ready
    Idle         ///< Execution is delayed until all jobs are dispatched
};

/**
 * @brief Systemd restart policy.
 */
enum class SystemdRestartPolicy {
    No,           ///< Service will not be restarted
    OnSuccess,    ///< Restart only when exit code is 0
    OnFailure,    ///< Restart only when exit code is non-zero
    OnAbnormal,   ///< Restart on signal, core dump, timeout, or watchdog
    OnWatchdog,   ///< Restart only on watchdog timeout
    OnAbort,      ///< Restart only on unclean signal termination
    Always        ///< Always restart
};

/**
 * @brief Reason for shutdown request.
 */
enum class ShutdownReason {
    Unknown,         ///< Unknown reason
    UserRequested,   ///< Shutdown requested by user/application
    SignalReceived,  ///< SIGTERM or SIGINT received
    SystemShutdown,  ///< System is shutting down
    ServiceStop,     ///< Service stop command from systemctl
    WatchdogTimeout, ///< Watchdog timeout
    Error            ///< Shutdown due to unrecoverable error
};

/**
 * @brief Service status.
 */
enum class ServiceStatus {
    Unknown,       ///< Status cannot be determined
    NotInstalled,  ///< Service unit file not found
    Stopped,       ///< Service is installed but not running
    Running,       ///< Service is running
    Starting,      ///< Service is starting
    Stopping,      ///< Service is stopping
    Failed         ///< Service has failed
};

/**
 * @brief Health status for service reporting.
 */
enum class HealthStatus {
    Healthy,     ///< Service is operating normally
    Degraded,    ///< Service is running but with reduced functionality
    Unhealthy    ///< Service is not functioning properly
};

/**
 * @brief Service error codes.
 */
enum class ServiceErrorCode {
    Success = 0,
    Unknown = 1,
    InvalidConfiguration = 100,
    FileWriteFailed = 101,
    FileReadFailed = 102,
    PermissionDenied = 103,
    SignalHandlerFailed = 200,
    PidFileError = 201,
    ProcessNotRunning = 202,
    NetworkError = 300,
    SystemdNotifyError = 400
};

// =============================================================================
// Data Structures
// =============================================================================

/**
 * @brief Service error information.
 */
struct ServiceError {
    ServiceErrorCode code;
    std::string message;
    int systemErrno;  ///< errno value if applicable

    ServiceError(ServiceErrorCode c = ServiceErrorCode::Unknown,
                 std::string msg = "",
                 int err = 0)
        : code(c), message(std::move(msg)), systemErrno(err) {}
};

/**
 * @brief Network interface information.
 */
struct NetworkInterfaceInfo {
    std::string name;                    ///< Interface name (e.g., "eth0", "lo")
    std::string displayName;             ///< Human-readable name
    std::vector<std::string> ipv4Addresses;  ///< IPv4 addresses
    std::vector<std::string> ipv6Addresses;  ///< IPv6 addresses
    bool isUp;                           ///< Interface is up
    bool isLoopback;                     ///< Is loopback interface
    bool supportsMulticast;              ///< Supports multicast
    uint32_t mtu;                        ///< Maximum transmission unit
    std::string macAddress;              ///< MAC address

    NetworkInterfaceInfo()
        : isUp(false), isLoopback(false), supportsMulticast(false), mtu(0) {}
};

/**
 * @brief Configuration for systemd service unit.
 */
struct SystemdServiceConfig {
    // [Unit] section
    std::string serviceName;          ///< Service name (without .service suffix)
    std::string description;          ///< Service description
    std::vector<std::string> after;   ///< Units to start after
    std::vector<std::string> before;  ///< Units to start before
    std::vector<std::string> wants;   ///< Weak dependencies
    std::vector<std::string> requires;  ///< Strong dependencies

    // [Service] section
    SystemdServiceType serviceType;   ///< Service type
    std::string executablePath;       ///< Full path to executable
    std::vector<std::string> arguments;  ///< Command line arguments
    std::string workingDirectory;     ///< Working directory
    std::string userName;             ///< Run as user
    std::string groupName;            ///< Run as group
    std::map<std::string, std::string> environmentVariables;  ///< Environment vars
    std::string environmentFile;      ///< Path to environment file
    std::string pidFile;              ///< PID file path (for forking type)
    std::string configFile;           ///< Config file (used with %i in templates)

    // Restart configuration
    SystemdRestartPolicy restart;     ///< Restart policy
    uint32_t restartSec;              ///< Seconds to wait before restart
    uint32_t timeoutStartSec;         ///< Startup timeout
    uint32_t timeoutStopSec;          ///< Stop timeout
    uint32_t watchdogSec;             ///< Watchdog timeout (for notify type)

    // Resource limits
    uint64_t limitNoFile;             ///< Max open files (0 = default)
    uint64_t limitNProc;              ///< Max processes (0 = default)

    // Security options
    bool privateTmp;                  ///< Private /tmp
    bool protectSystem;               ///< Read-only system directories
    bool protectHome;                 ///< Inaccessible home directories
    bool noNewPrivileges;             ///< Prevent privilege escalation

    // Standard output
    std::string standardOutput;       ///< Where to send stdout (journal, file:path, etc.)
    std::string standardError;        ///< Where to send stderr

    // Template unit support
    bool isTemplateUnit;              ///< If true, generate as template unit

    // [Install] section
    std::string wantedBy;             ///< Target that wants this service
    std::string alias;                ///< Alias name

    SystemdServiceConfig()
        : serviceType(SystemdServiceType::Simple)
        , restart(SystemdRestartPolicy::OnFailure)
        , restartSec(5)
        , timeoutStartSec(90)
        , timeoutStopSec(90)
        , watchdogSec(0)
        , limitNoFile(0)
        , limitNProc(0)
        , privateTmp(true)
        , protectSystem(true)
        , protectHome(true)
        , noNewPrivileges(true)
        , standardOutput("journal")
        , standardError("journal")
        , isTemplateUnit(false)
        , wantedBy("multi-user.target") {}
};

/**
 * @brief Runtime service information.
 */
struct ServiceInfo {
    ServiceStatus status;
    pid_t pid;                        ///< Process ID if running
    std::chrono::system_clock::time_point startTime;  ///< When service started
    uint64_t restartCount;            ///< Number of restarts
    std::string mainPid;              ///< Main PID as string
    std::string activeState;          ///< Systemd active state
    std::string subState;             ///< Systemd sub-state

    ServiceInfo()
        : status(ServiceStatus::Unknown)
        , pid(-1)
        , restartCount(0) {}
};

// =============================================================================
// Callback Types
// =============================================================================

using ShutdownCallback = std::function<void(ShutdownReason)>;
using ReloadCallback = std::function<void()>;
using UserSignalCallback = std::function<void()>;

// =============================================================================
// Interface Definition
// =============================================================================

/**
 * @brief Abstract interface for Linux service wrapper.
 *
 * This interface defines the contract for Linux-specific service
 * management functionality including systemd integration, signal handling,
 * and daemon operation support.
 */
class ILinuxServiceWrapper {
public:
    virtual ~ILinuxServiceWrapper() = default;

    // Systemd unit configuration
    virtual core::Result<std::string, ServiceError> generateSystemdUnit(
        const SystemdServiceConfig& config) = 0;
    virtual core::Result<void, ServiceError> saveUnitToFile(
        const SystemdServiceConfig& config, const std::string& path) = 0;

    // Network interfaces
    virtual core::Result<std::vector<NetworkInterfaceInfo>, ServiceError>
        enumerateNetworkInterfaces() = 0;
    virtual core::Result<bool, ServiceError> canBindToInterface(
        const std::string& interfaceNameOrAddress) = 0;

    // Signal handling
    virtual core::Result<void, ServiceError> installSignalHandlers() = 0;
    virtual core::Result<void, ServiceError> uninstallSignalHandlers() = 0;
    virtual void setShutdownCallback(ShutdownCallback callback) = 0;
    virtual void setReloadCallback(ReloadCallback callback) = 0;
    virtual void setUserSignalCallback(int signalNum, UserSignalCallback callback) = 0;

    // PID file management
    virtual core::Result<void, ServiceError> createPidFile(const std::string& path) = 0;
    virtual core::Result<void, ServiceError> removePidFile(const std::string& path) = 0;
    virtual core::Result<bool, ServiceError> checkPidFile(const std::string& path) = 0;

    // Shutdown management
    virtual bool isShutdownRequested() const = 0;
    virtual void requestShutdown(ShutdownReason reason) = 0;
    virtual ShutdownReason getShutdownReason() const = 0;
    virtual bool waitForShutdown(std::chrono::milliseconds timeout) = 0;
    virtual void setGracePeriod(std::chrono::seconds period) = 0;
    virtual std::chrono::seconds getGracePeriod() const = 0;

    // Service status
    virtual core::Result<ServiceStatus, ServiceError> getServiceStatus() = 0;
    virtual core::Result<ServiceInfo, ServiceError> getServiceInfo() = 0;
    virtual core::Result<void, ServiceError> reportHealthStatus(
        HealthStatus status, const std::string& message = "") = 0;

    // Systemd notification
    virtual core::Result<void, ServiceError> notifyReady() = 0;
    virtual core::Result<void, ServiceError> notifyStopping() = 0;
    virtual core::Result<void, ServiceError> notifyWatchdog() = 0;
    virtual core::Result<void, ServiceError> notifyStatus(const std::string& status) = 0;
};

// =============================================================================
// Linux Implementation
// =============================================================================

/**
 * @brief Linux-specific service wrapper implementation.
 *
 * Provides systemd service management, signal handling, PID file management,
 * network interface enumeration, and daemon operation support on Linux.
 *
 * ## Systemd Integration
 * Generates systemd service unit files for daemon operation. Supports:
 * - Simple, forking, and notify service types
 * - Template units for multiple instances (e.g., openrtmp@.service)
 * - Watchdog support for health monitoring
 * - sd_notify integration for ready/stopping notifications
 *
 * ## Network Interface Binding
 * Enumerates available network interfaces and validates binding capability.
 * Supports binding to specific interfaces or all interfaces (0.0.0.0).
 *
 * ## Signal Handling
 * Handles standard daemon signals:
 * - SIGTERM: Graceful shutdown (from systemctl stop)
 * - SIGINT: Graceful shutdown (from Ctrl+C)
 * - SIGHUP: Configuration reload
 * - SIGUSR1/SIGUSR2: User-defined actions
 *
 * ## PID File Management
 * Creates and manages PID files for:
 * - Process identification
 * - Preventing multiple instances
 * - Systemd process tracking
 *
 * ## Multiple Instance Support
 * Supports running multiple instances via systemd template units:
 * - Template unit: openrtmp@.service
 * - Instances: openrtmp@1935.service, openrtmp@1936.service
 * - Uses %i specifier for instance-specific configuration
 *
 * ## Thread Safety
 * All methods are thread-safe. Signal handlers use atomic operations
 * and signalfd for safe communication with the main application.
 */
class LinuxServiceWrapper : public ILinuxServiceWrapper {
public:
    LinuxServiceWrapper();
    ~LinuxServiceWrapper() override;

    // Non-copyable, non-movable
    LinuxServiceWrapper(const LinuxServiceWrapper&) = delete;
    LinuxServiceWrapper& operator=(const LinuxServiceWrapper&) = delete;

    // ==========================================================================
    // Systemd Unit Configuration (Requirement 7.5)
    // ==========================================================================

    /**
     * @brief Generate systemd service unit configuration.
     *
     * Creates a properly formatted systemd unit file content that can be
     * installed to /etc/systemd/system/ or /usr/lib/systemd/system/.
     *
     * @param config Service configuration
     * @return Generated unit file content or error
     */
    core::Result<std::string, ServiceError> generateSystemdUnit(
        const SystemdServiceConfig& config) override;

    /**
     * @brief Generate and save systemd unit configuration to a file.
     *
     * @param config Service configuration
     * @param path File path to save the unit file
     * @return Success or error
     */
    core::Result<void, ServiceError> saveUnitToFile(
        const SystemdServiceConfig& config, const std::string& path) override;

    // ==========================================================================
    // Network Interface Enumeration (Requirement 7.4)
    // ==========================================================================

    /**
     * @brief Enumerate all available network interfaces.
     *
     * Returns information about all network interfaces on the system,
     * including their IP addresses, status, and capabilities.
     *
     * @return List of network interfaces or error
     */
    core::Result<std::vector<NetworkInterfaceInfo>, ServiceError>
        enumerateNetworkInterfaces() override;

    /**
     * @brief Check if binding to a specific interface is possible.
     *
     * @param interfaceNameOrAddress Interface name (e.g., "eth0") or IP address
     * @return true if binding is possible, false otherwise
     */
    core::Result<bool, ServiceError> canBindToInterface(
        const std::string& interfaceNameOrAddress) override;

    // ==========================================================================
    // Signal Handling (Requirement 7.5)
    // ==========================================================================

    /**
     * @brief Install signal handlers for daemon operation.
     *
     * After installation:
     * - SIGTERM: Initiates graceful shutdown
     * - SIGINT: Initiates graceful shutdown
     * - SIGHUP: Triggers reload callback
     * - SIGUSR1: Triggers user signal 1 callback
     * - SIGUSR2: Triggers user signal 2 callback
     *
     * @return Success or error
     */
    core::Result<void, ServiceError> installSignalHandlers() override;

    /**
     * @brief Remove installed signal handlers.
     *
     * Restores default signal handling behavior.
     *
     * @return Success or error
     */
    core::Result<void, ServiceError> uninstallSignalHandlers() override;

    /**
     * @brief Register callback for shutdown events.
     *
     * @param callback Function called when shutdown is requested
     */
    void setShutdownCallback(ShutdownCallback callback) override;

    /**
     * @brief Register callback for reload events (SIGHUP).
     *
     * @param callback Function called when SIGHUP is received
     */
    void setReloadCallback(ReloadCallback callback) override;

    /**
     * @brief Register callback for user signals (SIGUSR1/SIGUSR2).
     *
     * @param signalNum Signal number (1 for SIGUSR1, 2 for SIGUSR2)
     * @param callback Function called when the signal is received
     */
    void setUserSignalCallback(int signalNum, UserSignalCallback callback) override;

    // ==========================================================================
    // PID File Management (Requirement 7.5)
    // ==========================================================================

    /**
     * @brief Create a PID file for this process.
     *
     * @param path Path to the PID file
     * @return Success or error
     */
    core::Result<void, ServiceError> createPidFile(const std::string& path) override;

    /**
     * @brief Remove the PID file.
     *
     * @param path Path to the PID file
     * @return Success or error
     */
    core::Result<void, ServiceError> removePidFile(const std::string& path) override;

    /**
     * @brief Check if a process is running based on PID file.
     *
     * @param path Path to the PID file
     * @return true if process is running, false otherwise
     */
    core::Result<bool, ServiceError> checkPidFile(const std::string& path) override;

    // ==========================================================================
    // Shutdown Management
    // ==========================================================================

    /**
     * @brief Check if shutdown has been requested.
     *
     * @return true if shutdown was requested
     */
    bool isShutdownRequested() const override;

    /**
     * @brief Request graceful shutdown.
     *
     * This can be called from any thread, including signal handlers.
     *
     * @param reason Reason for shutdown
     */
    void requestShutdown(ShutdownReason reason) override;

    /**
     * @brief Get the reason for the shutdown request.
     *
     * @return Shutdown reason
     */
    ShutdownReason getShutdownReason() const override;

    /**
     * @brief Wait for shutdown to be requested.
     *
     * Blocks until shutdown is requested or timeout expires.
     *
     * @param timeout Maximum time to wait
     * @return true if shutdown was requested, false if timeout
     */
    bool waitForShutdown(std::chrono::milliseconds timeout) override;

    /**
     * @brief Set the grace period for shutdown.
     *
     * @param period Grace period duration
     */
    void setGracePeriod(std::chrono::seconds period) override;

    /**
     * @brief Get the configured grace period.
     *
     * @return Grace period duration
     */
    std::chrono::seconds getGracePeriod() const override;

    // ==========================================================================
    // Service Status
    // ==========================================================================

    /**
     * @brief Get the current service status.
     *
     * @return Service status
     */
    core::Result<ServiceStatus, ServiceError> getServiceStatus() override;

    /**
     * @brief Get detailed service information.
     *
     * @return Service information
     */
    core::Result<ServiceInfo, ServiceError> getServiceInfo() override;

    /**
     * @brief Report health status to systemd.
     *
     * @param status Health status
     * @param message Optional status message
     * @return Success or error
     */
    core::Result<void, ServiceError> reportHealthStatus(
        HealthStatus status, const std::string& message = "") override;

    // ==========================================================================
    // Systemd Notification (sd_notify)
    // ==========================================================================

    /**
     * @brief Notify systemd that the service is ready.
     *
     * Should be called after initialization is complete for Type=notify services.
     *
     * @return Success or error
     */
    core::Result<void, ServiceError> notifyReady() override;

    /**
     * @brief Notify systemd that the service is stopping.
     *
     * Should be called when beginning shutdown sequence.
     *
     * @return Success or error
     */
    core::Result<void, ServiceError> notifyStopping() override;

    /**
     * @brief Send watchdog keepalive to systemd.
     *
     * Should be called periodically when WatchdogSec is configured.
     *
     * @return Success or error
     */
    core::Result<void, ServiceError> notifyWatchdog() override;

    /**
     * @brief Update service status message in systemd.
     *
     * This status is shown in 'systemctl status' output.
     *
     * @param status Status message
     * @return Success or error
     */
    core::Result<void, ServiceError> notifyStatus(const std::string& status) override;

private:
    // Signal handler (must be static for sigaction)
    static void signalHandler(int signum);

    // Helper methods
    std::string escapeSystemdString(const std::string& str) const;
    std::string getServiceTypeString(SystemdServiceType type) const;
    std::string getRestartPolicyString(SystemdRestartPolicy policy) const;
    bool sendNotify(const std::string& message);

    // Instance data
    std::atomic<bool> shutdownRequested_;
    std::atomic<ShutdownReason> shutdownReason_;
    std::chrono::seconds gracePeriod_;

    ShutdownCallback shutdownCallback_;
    ReloadCallback reloadCallback_;
    UserSignalCallback usr1Callback_;
    UserSignalCallback usr2Callback_;
    std::mutex callbackMutex_;

    bool signalHandlersInstalled_;
    std::string currentPidFile_;

    // Static instance pointer for signal handler
    static LinuxServiceWrapper* instance_;
    static std::mutex instanceMutex_;
};

} // namespace linux_pal
} // namespace pal
} // namespace openrtmp

#endif // __linux__

#endif // OPENRTMP_PAL_LINUX_SERVICE_WRAPPER_HPP
