// OpenRTMP - Cross-platform RTMP Server
// Darwin (macOS) Service Wrapper Interface
//
// This component provides macOS-specific service management functionality:
// - Launchd plist configuration generation
// - Local network permission handling
// - Network interface enumeration
// - Signal handling for graceful shutdown
//
// Requirements Covered: 7.4, 7.5, 7.7

#ifndef OPENRTMP_PAL_DARWIN_SERVICE_WRAPPER_HPP
#define OPENRTMP_PAL_DARWIN_SERVICE_WRAPPER_HPP

#if defined(__APPLE__)

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
namespace darwin {

// =============================================================================
// Enumerations
// =============================================================================

/**
 * @brief Network permission status on macOS.
 *
 * macOS Monterey and later require local network permission for services
 * that listen on network interfaces.
 */
enum class NetworkPermissionStatus {
    NotDetermined,  ///< Permission has not been requested yet
    Granted,        ///< Permission was granted by the user
    Denied,         ///< Permission was denied by the user
    Restricted      ///< Permission is restricted by system policy
};

/**
 * @brief Reason for shutdown request.
 */
enum class ShutdownReason {
    Unknown,         ///< Unknown reason
    UserRequested,   ///< Shutdown requested by user/application
    SignalReceived,  ///< SIGTERM, SIGINT, or SIGHUP received
    SystemShutdown,  ///< System is shutting down
    ServiceStop,     ///< Service stop command from launchctl
    Error            ///< Shutdown due to unrecoverable error
};

/**
 * @brief Service status.
 */
enum class ServiceStatus {
    Unknown,       ///< Status cannot be determined
    NotInstalled,  ///< Service is not installed
    Stopped,       ///< Service is installed but not running
    Running        ///< Service is running
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
    NetworkError = 300
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

    ServiceError(ServiceErrorCode c = ServiceErrorCode::Unknown,
                 std::string msg = "")
        : code(c), message(std::move(msg)) {}
};

/**
 * @brief Network interface information.
 */
struct NetworkInterfaceInfo {
    std::string name;                    ///< Interface name (e.g., "en0", "lo0")
    std::string displayName;             ///< Human-readable name
    std::vector<std::string> ipv4Addresses;  ///< IPv4 addresses
    std::vector<std::string> ipv6Addresses;  ///< IPv6 addresses
    bool isUp;                           ///< Interface is up
    bool isLoopback;                     ///< Is loopback interface
    bool supportsMulticast;              ///< Supports multicast
    uint32_t mtu;                        ///< Maximum transmission unit

    NetworkInterfaceInfo()
        : isUp(false), isLoopback(false), supportsMulticast(false), mtu(0) {}
};

/**
 * @brief Configuration for launchd service.
 */
struct ServiceConfig {
    std::string serviceName;          ///< Service identifier (e.g., "com.openrtmp.server")
    std::string executablePath;       ///< Full path to executable
    std::string workingDirectory;     ///< Working directory for the service
    std::vector<std::string> arguments;  ///< Command line arguments
    std::map<std::string, std::string> environmentVariables;  ///< Environment variables

    std::string standardOutPath;      ///< Path for stdout log
    std::string standardErrorPath;    ///< Path for stderr log

    bool runAtLoad;                   ///< Start service when loaded
    bool keepAlive;                   ///< Restart service if it exits
    bool abandonProcessGroup;         ///< Detach from process group

    int throttleInterval;             ///< Minimum seconds between restarts

    std::string userName;             ///< Run as user (optional)
    std::string groupName;            ///< Run as group (optional)

    ServiceConfig()
        : runAtLoad(true)
        , keepAlive(true)
        , abandonProcessGroup(false)
        , throttleInterval(10) {}
};

/**
 * @brief Runtime service information.
 */
struct ServiceInfo {
    ServiceStatus status;
    pid_t pid;                        ///< Process ID if running
    std::chrono::system_clock::time_point startTime;  ///< When service started
    uint64_t restartCount;            ///< Number of restarts

    ServiceInfo()
        : status(ServiceStatus::Unknown)
        , pid(-1)
        , restartCount(0) {}
};

// =============================================================================
// Callback Types
// =============================================================================

using ShutdownCallback = std::function<void(ShutdownReason)>;
using NetworkPermissionCallback = std::function<void(NetworkPermissionStatus)>;

// =============================================================================
// Interface Definition
// =============================================================================

/**
 * @brief Abstract interface for platform service wrappers.
 *
 * This interface defines the contract for platform-specific service
 * management functionality. The Darwin implementation provides macOS-specific
 * features including launchd integration and local network permissions.
 */
class IServiceWrapper {
public:
    virtual ~IServiceWrapper() = default;

    // Launchd configuration
    virtual core::Result<std::string, ServiceError> generateLaunchdPlist(
        const ServiceConfig& config) = 0;
    virtual core::Result<void, ServiceError> savePlistToFile(
        const ServiceConfig& config, const std::string& path) = 0;

    // Network interfaces
    virtual core::Result<std::vector<NetworkInterfaceInfo>, ServiceError>
        enumerateNetworkInterfaces() = 0;
    virtual core::Result<bool, ServiceError> canBindToInterface(
        const std::string& interfaceNameOrAddress) = 0;

    // Network permissions
    virtual core::Result<NetworkPermissionStatus, ServiceError>
        checkNetworkPermissionStatus() = 0;
    virtual void requestNetworkPermission(NetworkPermissionCallback callback) = 0;

    // Signal handling
    virtual core::Result<void, ServiceError> installSignalHandlers() = 0;
    virtual core::Result<void, ServiceError> uninstallSignalHandlers() = 0;
    virtual void setShutdownCallback(ShutdownCallback callback) = 0;

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
};

// =============================================================================
// Darwin Implementation
// =============================================================================

/**
 * @brief macOS-specific service wrapper implementation.
 *
 * Provides launchd service management, local network permission handling,
 * network interface enumeration, and Unix signal handling for graceful
 * shutdown on macOS.
 *
 * ## Launchd Integration
 * Generates property list (plist) configuration files for launchd,
 * the macOS service manager. Services can be installed to:
 * - /Library/LaunchDaemons (system-wide, requires root)
 * - ~/Library/LaunchAgents (user-specific)
 *
 * ## Network Permissions
 * macOS Monterey (12.0) and later require explicit permission for apps
 * to access the local network. This wrapper provides APIs to check and
 * request these permissions through the standard system dialogs.
 *
 * ## Signal Handling
 * Handles SIGTERM (graceful stop), SIGINT (interrupt), and SIGHUP
 * (reload configuration) signals for daemon operation.
 *
 * ## Thread Safety
 * All methods are thread-safe. Signal handlers use atomic operations
 * for coordination with the main application.
 */
class DarwinServiceWrapper : public IServiceWrapper {
public:
    DarwinServiceWrapper();
    ~DarwinServiceWrapper() override;

    // Non-copyable, non-movable
    DarwinServiceWrapper(const DarwinServiceWrapper&) = delete;
    DarwinServiceWrapper& operator=(const DarwinServiceWrapper&) = delete;

    // ==========================================================================
    // Launchd Configuration (Requirement 7.5)
    // ==========================================================================

    /**
     * @brief Generate launchd plist XML configuration.
     *
     * Creates a properly formatted property list (plist) XML document
     * that can be used with launchctl to manage the service.
     *
     * @param config Service configuration
     * @return Generated plist XML string or error
     */
    core::Result<std::string, ServiceError> generateLaunchdPlist(
        const ServiceConfig& config) override;

    /**
     * @brief Generate and save plist configuration to a file.
     *
     * @param config Service configuration
     * @param path File path to save the plist
     * @return Success or error
     */
    core::Result<void, ServiceError> savePlistToFile(
        const ServiceConfig& config, const std::string& path) override;

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
     * @param interfaceNameOrAddress Interface name (e.g., "en0") or IP address
     * @return true if binding is possible, false otherwise
     */
    core::Result<bool, ServiceError> canBindToInterface(
        const std::string& interfaceNameOrAddress) override;

    // ==========================================================================
    // Network Permissions (Requirement 7.7)
    // ==========================================================================

    /**
     * @brief Check current local network permission status.
     *
     * @return Current permission status
     */
    core::Result<NetworkPermissionStatus, ServiceError>
        checkNetworkPermissionStatus() override;

    /**
     * @brief Request local network permission from the user.
     *
     * On macOS 12+, this triggers the system permission dialog.
     * The callback is invoked when the user responds or if the
     * permission is already determined.
     *
     * @param callback Function called with the permission result
     */
    void requestNetworkPermission(NetworkPermissionCallback callback) override;

    // ==========================================================================
    // Signal Handling (Requirement 7.7)
    // ==========================================================================

    /**
     * @brief Install signal handlers for SIGTERM, SIGINT, and SIGHUP.
     *
     * After installation:
     * - SIGTERM: Initiates graceful shutdown
     * - SIGINT: Initiates graceful shutdown (Ctrl+C)
     * - SIGHUP: Triggers reload callback (if registered)
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
     * This is the time allowed for active operations to complete
     * before forcing shutdown.
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

private:
    // Signal handler (must be static for sigaction)
    static void signalHandler(int signum);

    // Helper methods
    std::string escapePlistString(const std::string& str) const;
    bool isValidPlistIdentifier(const std::string& identifier) const;

    // Instance data
    std::atomic<bool> shutdownRequested_;
    std::atomic<ShutdownReason> shutdownReason_;
    std::chrono::seconds gracePeriod_;

    ShutdownCallback shutdownCallback_;
    std::mutex callbackMutex_;

    bool signalHandlersInstalled_;

    // Static instance pointer for signal handler
    static DarwinServiceWrapper* instance_;
    static std::mutex instanceMutex_;
};

} // namespace darwin
} // namespace pal
} // namespace openrtmp

#endif // __APPLE__

#endif // OPENRTMP_PAL_DARWIN_SERVICE_WRAPPER_HPP
