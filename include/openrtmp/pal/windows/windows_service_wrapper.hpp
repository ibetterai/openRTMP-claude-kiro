// OpenRTMP - Cross-platform RTMP Server
// Windows Service Wrapper Interface
//
// This component provides Windows-specific service management functionality:
// - Windows Service Control Manager (SCM) integration
// - Windows Firewall rule management for port exceptions
// - Architecture detection (x64, ARM64)
// - Service installation and uninstallation
// - Console mode for debugging
//
// Requirements Covered: 7.4, 7.5, 7.6

#ifndef OPENRTMP_PAL_WINDOWS_SERVICE_WRAPPER_HPP
#define OPENRTMP_PAL_WINDOWS_SERVICE_WRAPPER_HPP

#include "openrtmp/core/result.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace openrtmp {
namespace pal {
namespace windows {

// =============================================================================
// Enumerations
// =============================================================================

/**
 * @brief Processor architecture types.
 *
 * Used to detect and report the current processor architecture.
 * Supports both x64 and ARM64 as per requirement 7.6.
 */
enum class ProcessorArchitecture {
    x86,        ///< 32-bit x86
    x64,        ///< 64-bit x86-64 (AMD64)
    ARM,        ///< 32-bit ARM
    ARM64,      ///< 64-bit ARM (AArch64)
    Unknown     ///< Unknown architecture
};

/**
 * @brief Windows service start type.
 */
enum class ServiceStartType {
    Boot,           ///< Driver loaded by system loader
    System,         ///< Driver loaded during kernel init
    AutoStart,      ///< Service started automatically at boot
    DemandStart,    ///< Service started on demand
    Disabled        ///< Service is disabled
};

/**
 * @brief Windows service current state.
 */
enum class ServiceState {
    Stopped = 1,            ///< Service is stopped
    StartPending = 2,       ///< Service is starting
    StopPending = 3,        ///< Service is stopping
    Running = 4,            ///< Service is running
    ContinuePending = 5,    ///< Service continue is pending
    PausePending = 6,       ///< Service pause is pending
    Paused = 7,             ///< Service is paused
    Unknown = 0             ///< Unknown state
};

/**
 * @brief Service control codes received from SCM.
 */
enum class ServiceControlCode {
    Stop = 1,               ///< Stop the service
    Pause = 2,              ///< Pause the service
    Continue = 3,           ///< Continue the paused service
    Interrogate = 4,        ///< Report current status
    Shutdown = 5,           ///< System shutdown
    ParamChange = 6,        ///< Service parameters changed
    NetBindAdd = 7,         ///< Network binding added
    NetBindRemove = 8,      ///< Network binding removed
    NetBindEnable = 9,      ///< Network binding enabled
    NetBindDisable = 10,    ///< Network binding disabled
    DeviceEvent = 11,       ///< Device event
    HardwareProfileChange = 12,  ///< Hardware profile changed
    PowerEvent = 13,        ///< Power event
    SessionChange = 14,     ///< Session change event
    PreShutdown = 15,       ///< Pre-shutdown notification
    TimeChange = 16,        ///< System time changed
    TriggerEvent = 32       ///< Trigger event
};

/**
 * @brief Service accepted controls bitmask.
 */
enum ServiceAcceptedControl : uint32_t {
    Stop = 0x00000001,
    PauseContinue = 0x00000002,
    Shutdown = 0x00000004,
    ParamChange = 0x00000008,
    NetBindChange = 0x00000010,
    HardwareProfileChange = 0x00000020,
    PowerEvent = 0x00000040,
    SessionChange = 0x00000080,
    PreShutdown = 0x00000100,
    TimeChange = 0x00000200,
    TriggerEvent = 0x00000400
};

/**
 * @brief Reason for shutdown request.
 */
enum class ShutdownReason {
    Unknown,            ///< Unknown reason
    UserRequested,      ///< Shutdown requested by user/application
    ServiceStop,        ///< Service stop from SCM
    SystemShutdown,     ///< System is shutting down
    PreShutdown,        ///< Pre-shutdown notification
    Error               ///< Shutdown due to error
};

/**
 * @brief Firewall protocol type.
 */
enum class FirewallProtocol {
    TCP,    ///< TCP protocol
    UDP,    ///< UDP protocol
    Any     ///< Any protocol
};

/**
 * @brief Firewall rule direction.
 */
enum class FirewallDirection {
    Inbound,    ///< Inbound traffic
    Outbound    ///< Outbound traffic
};

/**
 * @brief Firewall rule action.
 */
enum class FirewallAction {
    Allow,  ///< Allow traffic
    Block   ///< Block traffic
};

/**
 * @brief Event log entry type.
 */
enum class EventLogType {
    Information,    ///< Information message
    Warning,        ///< Warning message
    Error           ///< Error message
};

/**
 * @brief Service error codes.
 */
enum class ServiceErrorCode {
    Success = 0,
    Unknown = 1,
    InvalidConfiguration = 100,
    ServiceNotFound = 101,
    AccessDenied = 102,
    ServiceAlreadyExists = 103,
    ServiceMarkedForDeletion = 104,
    NotRunningAsService = 200,
    SCMConnectionFailed = 201,
    ControlHandlerFailed = 202,
    FirewallError = 300,
    FirewallRuleExists = 301,
    FirewallRuleNotFound = 302,
    EventLogError = 400
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
    uint32_t win32ErrorCode;

    ServiceError(ServiceErrorCode c = ServiceErrorCode::Unknown,
                 std::string msg = "",
                 uint32_t win32Code = 0)
        : code(c), message(std::move(msg)), win32ErrorCode(win32Code) {}
};

/**
 * @brief Windows service configuration.
 */
struct WindowsServiceConfig {
    std::string serviceName;          ///< Internal service name (no spaces)
    std::string displayName;          ///< Display name shown in services.msc
    std::string description;          ///< Service description
    std::string executablePath;       ///< Full path to executable
    std::vector<std::string> arguments;  ///< Command line arguments
    std::string userName;             ///< Run as user (empty = LocalSystem)
    std::string password;             ///< Password for user account
    ServiceStartType startType;       ///< Service start type
    std::vector<std::string> dependencies;  ///< Service dependencies
    bool delayedAutoStart;            ///< Enable delayed auto-start

    WindowsServiceConfig()
        : serviceName("OpenRTMPServer")
        , displayName("OpenRTMP RTMP Server")
        , startType(ServiceStartType::AutoStart)
        , delayedAutoStart(false) {}
};

/**
 * @brief Firewall rule configuration.
 */
struct FirewallRuleConfig {
    std::string ruleName;             ///< Rule name
    std::string description;          ///< Rule description
    std::string applicationPath;      ///< Application path (optional)
    uint16_t port;                    ///< Port number
    FirewallProtocol protocol;        ///< Protocol type
    FirewallDirection direction;      ///< Traffic direction
    FirewallAction action;            ///< Rule action
    std::string remoteAddresses;      ///< Remote addresses (e.g., "*" or "LocalSubnet")
    std::string localAddresses;       ///< Local addresses (e.g., "*")
    std::vector<std::string> profiles;  ///< Firewall profiles (Domain, Private, Public)
    bool enabled;                     ///< Whether rule is enabled

    FirewallRuleConfig()
        : port(1935)
        , protocol(FirewallProtocol::TCP)
        , direction(FirewallDirection::Inbound)
        , action(FirewallAction::Allow)
        , remoteAddresses("*")
        , localAddresses("*")
        , profiles({"Domain", "Private", "Public"})
        , enabled(true) {}
};

/**
 * @brief Service status information.
 */
struct ServiceStatusInfo {
    ServiceState state;               ///< Current service state
    uint32_t acceptedControls;        ///< Accepted control codes
    uint32_t win32ExitCode;           ///< Win32 exit code
    uint32_t serviceSpecificExitCode; ///< Service-specific exit code
    uint32_t checkPoint;              ///< Checkpoint value for pending ops
    uint32_t waitHint;                ///< Wait hint in milliseconds

    ServiceStatusInfo()
        : state(ServiceState::Stopped)
        , acceptedControls(0)
        , win32ExitCode(0)
        , serviceSpecificExitCode(0)
        , checkPoint(0)
        , waitHint(0) {}
};

// =============================================================================
// Callback Types
// =============================================================================

using ServiceControlCallback = std::function<void(ServiceControlCode)>;
using ShutdownCallback = std::function<void(ShutdownReason)>;
using ServiceMainCallback = std::function<bool()>;  // Returns true on success
using ServiceStopCallback = std::function<void()>;

// =============================================================================
// Interface Definition
// =============================================================================

/**
 * @brief Abstract interface for Windows service management.
 *
 * This interface defines the contract for Windows-specific service
 * management functionality including SCM integration, firewall rules,
 * and service lifecycle management.
 */
class IWindowsServiceWrapper {
public:
    virtual ~IWindowsServiceWrapper() = default;

    // Architecture detection
    virtual ProcessorArchitecture getProcessorArchitecture() const = 0;
    virtual std::string getProcessorArchitectureString() const = 0;
    virtual bool is64BitProcess() const = 0;

    // Service configuration
    virtual core::Result<void, ServiceError> validateServiceConfig(
        const WindowsServiceConfig& config) = 0;

    // Service installation
    virtual core::Result<void, ServiceError> installService(
        const WindowsServiceConfig& config) = 0;
    virtual core::Result<void, ServiceError> uninstallService(
        const std::string& serviceName) = 0;
    virtual std::string generateInstallCommand(
        const WindowsServiceConfig& config) const = 0;
    virtual std::string generateUninstallCommand(
        const std::string& serviceName) const = 0;

    // Service control
    virtual core::Result<void, ServiceError> startService(
        const std::string& serviceName) = 0;
    virtual core::Result<void, ServiceError> stopService(
        const std::string& serviceName) = 0;
    virtual core::Result<ServiceStatusInfo, ServiceError> getServiceStatus() = 0;
    virtual core::Result<void, ServiceError> setServiceStatus(
        const ServiceStatusInfo& status) = 0;
    virtual ServiceState getCurrentServiceState() const = 0;

    // SCM event handling
    virtual core::Result<void, ServiceError> registerControlHandler() = 0;
    virtual void setControlCallback(ServiceControlCallback callback) = 0;

    // Firewall management
    virtual core::Result<void, ServiceError> validateFirewallRule(
        const FirewallRuleConfig& rule) = 0;
    virtual core::Result<void, ServiceError> addFirewallRule(
        const FirewallRuleConfig& rule) = 0;
    virtual core::Result<void, ServiceError> removeFirewallRule(
        const std::string& ruleName) = 0;
    virtual core::Result<bool, ServiceError> firewallRuleExists(
        const std::string& ruleName) = 0;

    // Service execution modes
    virtual bool isRunningAsService() const = 0;
    virtual bool isConsoleApplication() const = 0;
    virtual core::Result<void, ServiceError> runAsService(
        const std::string& serviceName,
        ServiceMainCallback mainCallback,
        ServiceStopCallback stopCallback) = 0;
    virtual core::Result<void, ServiceError> runAsConsole(
        ServiceMainCallback mainCallback,
        ServiceStopCallback stopCallback) = 0;

    // Shutdown management
    virtual bool isShutdownRequested() const = 0;
    virtual void requestShutdown(ShutdownReason reason) = 0;
    virtual ShutdownReason getShutdownReason() const = 0;
    virtual bool waitForShutdown(std::chrono::milliseconds timeout) = 0;
    virtual void setGracePeriod(std::chrono::seconds period) = 0;
    virtual std::chrono::seconds getGracePeriod() const = 0;
    virtual void setShutdownCallback(ShutdownCallback callback) = 0;

    // Event logging
    virtual void logEvent(EventLogType type, uint32_t eventId,
                         const std::string& message) = 0;
};

// =============================================================================
// Windows Implementation
// =============================================================================

/**
 * @brief Windows-specific service wrapper implementation.
 *
 * Provides Windows Service Control Manager integration, Windows Firewall
 * rule management, architecture detection, and console mode support for
 * debugging.
 *
 * ## Windows Service Integration
 * Implements full SCM integration allowing the RTMP server to run as a
 * Windows service with proper start/stop handling, status reporting,
 * and event logging.
 *
 * ## Windows Firewall Integration (Requirement 7.6)
 * Manages Windows Firewall rules to allow RTMP traffic through the
 * firewall during service installation. Creates inbound TCP rule for
 * the configured RTMP port.
 *
 * ## Architecture Support (Requirement 7.6)
 * Supports both x64 and ARM64 Windows 11 installations, with runtime
 * detection of the current processor architecture.
 *
 * ## Console Mode
 * Supports running as a console application for debugging and testing
 * purposes, with the same lifecycle callbacks as service mode.
 *
 * ## Thread Safety
 * All methods are thread-safe. SCM callbacks are serialized using
 * internal synchronization.
 */
class WindowsServiceWrapper : public IWindowsServiceWrapper {
public:
    WindowsServiceWrapper();
    ~WindowsServiceWrapper() override;

    // Non-copyable, non-movable
    WindowsServiceWrapper(const WindowsServiceWrapper&) = delete;
    WindowsServiceWrapper& operator=(const WindowsServiceWrapper&) = delete;

    // ==========================================================================
    // Architecture Detection (Requirement 7.6)
    // ==========================================================================

    /**
     * @brief Get the current processor architecture.
     *
     * @return Processor architecture (x64, ARM64, etc.)
     */
    ProcessorArchitecture getProcessorArchitecture() const override;

    /**
     * @brief Get processor architecture as string.
     *
     * @return Architecture string (e.g., "x64", "ARM64")
     */
    std::string getProcessorArchitectureString() const override;

    /**
     * @brief Check if running as 64-bit process.
     *
     * @return true if 64-bit process
     */
    bool is64BitProcess() const override;

    // ==========================================================================
    // Service Configuration
    // ==========================================================================

    /**
     * @brief Validate service configuration.
     *
     * @param config Service configuration to validate
     * @return Success or validation error
     */
    core::Result<void, ServiceError> validateServiceConfig(
        const WindowsServiceConfig& config) override;

    // ==========================================================================
    // Service Installation (Requirement 7.5)
    // ==========================================================================

    /**
     * @brief Install the service in SCM.
     *
     * Requires administrative privileges.
     *
     * @param config Service configuration
     * @return Success or error
     */
    core::Result<void, ServiceError> installService(
        const WindowsServiceConfig& config) override;

    /**
     * @brief Uninstall the service from SCM.
     *
     * Requires administrative privileges. Service must be stopped.
     *
     * @param serviceName Name of service to uninstall
     * @return Success or error
     */
    core::Result<void, ServiceError> uninstallService(
        const std::string& serviceName) override;

    /**
     * @brief Generate sc.exe command for service installation.
     *
     * @param config Service configuration
     * @return Command line string
     */
    std::string generateInstallCommand(
        const WindowsServiceConfig& config) const override;

    /**
     * @brief Generate sc.exe command for service uninstallation.
     *
     * @param serviceName Service name
     * @return Command line string
     */
    std::string generateUninstallCommand(
        const std::string& serviceName) const override;

    // ==========================================================================
    // Service Control
    // ==========================================================================

    /**
     * @brief Start the service.
     *
     * @param serviceName Service name
     * @return Success or error
     */
    core::Result<void, ServiceError> startService(
        const std::string& serviceName) override;

    /**
     * @brief Stop the service.
     *
     * @param serviceName Service name
     * @return Success or error
     */
    core::Result<void, ServiceError> stopService(
        const std::string& serviceName) override;

    /**
     * @brief Get current service status.
     *
     * @return Service status or error
     */
    core::Result<ServiceStatusInfo, ServiceError> getServiceStatus() override;

    /**
     * @brief Set service status (for reporting to SCM).
     *
     * @param status Status to report
     * @return Success or error
     */
    core::Result<void, ServiceError> setServiceStatus(
        const ServiceStatusInfo& status) override;

    /**
     * @brief Get current service state.
     *
     * @return Current service state
     */
    ServiceState getCurrentServiceState() const override;

    // ==========================================================================
    // SCM Event Handling (Requirement 7.5)
    // ==========================================================================

    /**
     * @brief Register the service control handler with SCM.
     *
     * @return Success or error
     */
    core::Result<void, ServiceError> registerControlHandler() override;

    /**
     * @brief Set callback for service control events.
     *
     * @param callback Function called when control events are received
     */
    void setControlCallback(ServiceControlCallback callback) override;

    // ==========================================================================
    // Firewall Management (Requirement 7.6)
    // ==========================================================================

    /**
     * @brief Validate firewall rule configuration.
     *
     * @param rule Rule configuration to validate
     * @return Success or error
     */
    core::Result<void, ServiceError> validateFirewallRule(
        const FirewallRuleConfig& rule) override;

    /**
     * @brief Add a Windows Firewall rule.
     *
     * Requires administrative privileges.
     *
     * @param rule Firewall rule configuration
     * @return Success or error
     */
    core::Result<void, ServiceError> addFirewallRule(
        const FirewallRuleConfig& rule) override;

    /**
     * @brief Remove a Windows Firewall rule.
     *
     * @param ruleName Name of rule to remove
     * @return Success or error
     */
    core::Result<void, ServiceError> removeFirewallRule(
        const std::string& ruleName) override;

    /**
     * @brief Check if a firewall rule exists.
     *
     * @param ruleName Rule name to check
     * @return true if exists, false if not, or error
     */
    core::Result<bool, ServiceError> firewallRuleExists(
        const std::string& ruleName) override;

    // ==========================================================================
    // Service Execution Modes
    // ==========================================================================

    /**
     * @brief Check if running as a Windows service.
     *
     * @return true if running as service
     */
    bool isRunningAsService() const override;

    /**
     * @brief Check if running as console application.
     *
     * @return true if running as console app
     */
    bool isConsoleApplication() const override;

    /**
     * @brief Run as Windows service.
     *
     * Connects to SCM and runs the service main loop.
     *
     * @param serviceName Service name
     * @param mainCallback Called after service starts
     * @param stopCallback Called when service should stop
     * @return Success or error
     */
    core::Result<void, ServiceError> runAsService(
        const std::string& serviceName,
        ServiceMainCallback mainCallback,
        ServiceStopCallback stopCallback) override;

    /**
     * @brief Run as console application.
     *
     * Runs the service in console mode for debugging.
     *
     * @param mainCallback Called to start the service
     * @param stopCallback Called when service should stop
     * @return Success or error
     */
    core::Result<void, ServiceError> runAsConsole(
        ServiceMainCallback mainCallback,
        ServiceStopCallback stopCallback) override;

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
     * @param reason Reason for shutdown
     */
    void requestShutdown(ShutdownReason reason) override;

    /**
     * @brief Get the reason for shutdown request.
     *
     * @return Shutdown reason
     */
    ShutdownReason getShutdownReason() const override;

    /**
     * @brief Wait for shutdown to be requested.
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

    /**
     * @brief Register callback for shutdown events.
     *
     * @param callback Function called when shutdown is requested
     */
    void setShutdownCallback(ShutdownCallback callback) override;

    // ==========================================================================
    // Event Logging
    // ==========================================================================

    /**
     * @brief Log an event to Windows Event Log.
     *
     * @param type Event type (Information, Warning, Error)
     * @param eventId Event ID
     * @param message Event message
     */
    void logEvent(EventLogType type, uint32_t eventId,
                 const std::string& message) override;

private:
    // Helper methods
    ProcessorArchitecture detectArchitecture() const;
    std::string quotePathIfNeeded(const std::string& path) const;

#if defined(_WIN32)
    // Windows-specific members
    static void WINAPI serviceMain(DWORD argc, LPWSTR* argv);
    static DWORD WINAPI serviceControlHandler(
        DWORD control, DWORD eventType, LPVOID eventData, LPVOID context);

    // Static instance for service callbacks
    static WindowsServiceWrapper* instance_;
    static std::mutex instanceMutex_;

    // Service handles
    void* serviceStatusHandle_;  // SERVICE_STATUS_HANDLE
    void* eventLogHandle_;       // HANDLE for event log
#endif

    // Service callbacks
    ServiceControlCallback controlCallback_;
    ShutdownCallback shutdownCallback_;
    ServiceMainCallback mainCallback_;
    ServiceStopCallback stopCallback_;

    // State
    mutable std::mutex stateMutex_;
    std::atomic<bool> shutdownRequested_;
    std::atomic<ShutdownReason> shutdownReason_;
    std::atomic<ServiceState> currentState_;
    std::chrono::seconds gracePeriod_;

    // Detected architecture
    mutable ProcessorArchitecture cachedArchitecture_;
    mutable bool architectureDetected_;
};

} // namespace windows
} // namespace pal
} // namespace openrtmp

#endif // OPENRTMP_PAL_WINDOWS_SERVICE_WRAPPER_HPP
