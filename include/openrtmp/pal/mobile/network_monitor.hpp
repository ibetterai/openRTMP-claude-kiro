// OpenRTMP - Cross-platform RTMP Server
// Mobile Network Monitor Interface
//
// This component provides network monitoring for mobile platforms:
// - Detect network connectivity changes within 2 seconds
// - Attempt interface rebind on WiFi to cellular transition when allowed
// - Notify clients of new server address when interface changes
// - Maintain connection state for 30 seconds during network loss
// - Support configuration to restrict to WiFi-only or cellular-only
// - Warn users about cellular data usage through API
//
// Requirements Covered: 11.1, 11.2, 11.3, 11.4, 11.5, 11.6

#ifndef OPENRTMP_PAL_MOBILE_NETWORK_MONITOR_HPP
#define OPENRTMP_PAL_MOBILE_NETWORK_MONITOR_HPP

#include "openrtmp/core/result.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace openrtmp {
namespace pal {
namespace mobile {

// =============================================================================
// Enumerations
// =============================================================================

/**
 * @brief Network connection types.
 */
enum class NetworkType {
    None,       ///< No network connection
    WiFi,       ///< WiFi network
    Cellular,   ///< Cellular data network (3G/4G/5G)
    Ethernet    ///< Wired ethernet connection
};

/**
 * @brief Flags for allowed network types (Requirement 11.5).
 *
 * Can be combined using bitwise OR to allow multiple network types.
 */
enum class NetworkTypeFlags : uint32_t {
    None = 0x00,
    WiFi = 0x01,
    Cellular = 0x02,
    Ethernet = 0x04,
    Any = WiFi | Cellular | Ethernet
};

// Bitwise operators for NetworkTypeFlags
inline NetworkTypeFlags operator|(NetworkTypeFlags a, NetworkTypeFlags b) {
    return static_cast<NetworkTypeFlags>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline NetworkTypeFlags operator&(NetworkTypeFlags a, NetworkTypeFlags b) {
    return static_cast<NetworkTypeFlags>(
        static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

/**
 * @brief Network monitor error codes.
 */
enum class NetworkMonitorErrorCode {
    Success = 0,
    Unknown = 1,
    AlreadyMonitoring = 100,
    NotMonitoring = 101,
    NetworkTypeNotAllowed = 200,
    RebindFailed = 201,
    InterfaceUnavailable = 202,
    PermissionDenied = 300,
    PlatformNotSupported = 400
};

// =============================================================================
// Data Structures
// =============================================================================

/**
 * @brief Network monitor error information.
 */
struct NetworkMonitorError {
    NetworkMonitorErrorCode code;
    std::string message;

    NetworkMonitorError(NetworkMonitorErrorCode c = NetworkMonitorErrorCode::Unknown,
                        std::string msg = "")
        : code(c), message(std::move(msg)) {}
};

/**
 * @brief Current network state information.
 */
struct NetworkState {
    bool isConnected = false;           ///< Whether network is connected
    NetworkType type = NetworkType::None; ///< Current network type
    std::string interfaceName;          ///< Network interface name (e.g., "en0", "pdp_ip0")
    std::optional<std::string> ipAddress; ///< Current IP address
    int signalStrength = 0;             ///< Signal strength (0-100)
    bool isMetered = false;             ///< Whether connection is metered (cellular)
};

/**
 * @brief Network change event information.
 */
struct NetworkChangeEvent {
    NetworkType oldType;                ///< Previous network type
    NetworkType newType;                ///< New network type
    bool wasConnected;                  ///< Was connected before change
    bool isConnected;                   ///< Is connected after change
    std::chrono::steady_clock::time_point timestamp; ///< Event timestamp
};

// =============================================================================
// Callback Types
// =============================================================================

/**
 * @brief Callback for network change events (Requirement 11.1).
 *
 * @param event Details of the network change
 */
using NetworkChangeCallback = std::function<void(const NetworkChangeEvent& event)>;

/**
 * @brief Callback for server address changes (Requirement 11.3).
 *
 * @param newAddress The new server address
 */
using AddressChangeCallback = std::function<void(const std::string& newAddress)>;

/**
 * @brief Callback for cellular data usage warnings (Requirement 11.6).
 *
 * Called when the connection transitions to cellular data to warn users
 * about potential data charges.
 */
using CellularWarningCallback = std::function<void()>;

// =============================================================================
// Interface Definition
// =============================================================================

/**
 * @brief Abstract interface for mobile network monitoring.
 *
 * This interface defines the contract for mobile-specific network monitoring
 * features. Implementations provide platform-specific network state tracking
 * for iOS and Android.
 *
 * ## Network Change Detection (Requirement 11.1)
 * Detect network connectivity changes within 2 seconds. This includes:
 * - WiFi to cellular transitions
 * - Cellular to WiFi transitions
 * - Network disconnection events
 * - Network reconnection events
 *
 * ## Interface Rebinding (Requirement 11.2)
 * Attempt interface rebind on WiFi to cellular transition when allowed.
 * The server can automatically switch to cellular data when WiFi is lost,
 * if configured to do so.
 *
 * ## Address Change Notification (Requirement 11.3)
 * Notify clients of new server address when interface changes. This allows
 * connected clients to reconnect to the new address.
 *
 * ## Connection State Preservation (Requirement 11.4)
 * Maintain connection state for 30 seconds during network loss. This grace
 * period allows temporary network disruptions without dropping clients.
 *
 * ## Network Type Restriction (Requirement 11.5)
 * Support configuration to restrict to WiFi-only or cellular-only. Users
 * can prevent the server from using cellular data to avoid charges.
 *
 * ## Cellular Data Warning (Requirement 11.6)
 * Warn users about cellular data usage through API. When switching to
 * cellular, users are notified about potential data charges.
 */
class INetworkMonitor {
public:
    virtual ~INetworkMonitor() = default;

    // =========================================================================
    // Lifecycle
    // =========================================================================

    /**
     * @brief Start network monitoring.
     *
     * Begins listening for network state changes. Must be called before
     * any network state information is available.
     *
     * @return Success or error
     */
    virtual core::Result<void, NetworkMonitorError> startMonitoring() = 0;

    /**
     * @brief Stop network monitoring.
     *
     * Stops listening for network changes and releases resources.
     */
    virtual void stopMonitoring() = 0;

    /**
     * @brief Check if monitoring is active.
     *
     * @return true if currently monitoring network state
     */
    virtual bool isMonitoringActive() const = 0;

    // =========================================================================
    // Network State (Requirement 11.1)
    // =========================================================================

    /**
     * @brief Get current network state.
     *
     * Returns comprehensive information about the current network connection.
     *
     * @return Current network state
     */
    virtual NetworkState getCurrentState() const = 0;

    /**
     * @brief Get the active network type.
     *
     * @return Current network type (WiFi, Cellular, Ethernet, or None)
     */
    virtual NetworkType getActiveNetworkType() const = 0;

    /**
     * @brief Check if currently connected to any network.
     *
     * @return true if connected to a network
     */
    virtual bool isConnected() const = 0;

    // =========================================================================
    // Network Configuration (Requirement 11.5)
    // =========================================================================

    /**
     * @brief Set allowed network types.
     *
     * Configure which network types the server is allowed to use.
     * This can restrict to WiFi-only or cellular-only operation.
     *
     * @param allowed Bitwise flags of allowed network types
     */
    virtual void setAllowedNetworks(NetworkTypeFlags allowed) = 0;

    /**
     * @brief Get currently allowed network types.
     *
     * @return Bitwise flags of allowed network types
     */
    virtual NetworkTypeFlags getAllowedNetworks() const = 0;

    /**
     * @brief Check if a specific network type is allowed.
     *
     * @param type Network type to check
     * @return true if the network type is allowed
     */
    virtual bool isNetworkTypeAllowed(NetworkType type) const = 0;

    // =========================================================================
    // Connection State Preservation (Requirement 11.4)
    // =========================================================================

    /**
     * @brief Set the connection state grace period.
     *
     * During network loss, connection state is maintained for this duration
     * to allow temporary disruptions without dropping clients.
     *
     * @param duration Grace period duration (default 30 seconds)
     */
    virtual void setConnectionStateGracePeriod(std::chrono::milliseconds duration) = 0;

    /**
     * @brief Get the connection state grace period.
     *
     * @return Current grace period duration
     */
    virtual std::chrono::milliseconds getConnectionStateGracePeriod() const = 0;

    /**
     * @brief Check if currently in grace period.
     *
     * @return true if disconnected but within grace period
     */
    virtual bool isInGracePeriod() const = 0;

    /**
     * @brief Get remaining grace period time.
     *
     * @return Remaining grace period, or 0 if not in grace period
     */
    virtual std::chrono::milliseconds getRemainingGracePeriod() const = 0;

    // =========================================================================
    // Interface Rebinding (Requirement 11.2)
    // =========================================================================

    /**
     * @brief Manually rebind to a specific network interface.
     *
     * Forces the server to switch to a different network interface.
     * Fails if the specified network type is not allowed.
     *
     * @param type Network type to bind to
     * @return Success or error
     */
    virtual core::Result<void, NetworkMonitorError> rebindToInterface(NetworkType type) = 0;

    /**
     * @brief Attempt automatic rebind to available network.
     *
     * Tries to find and bind to an available network based on the
     * allowed network types configuration.
     *
     * @return true if successfully rebound to a network
     */
    virtual bool attemptAutomaticRebind() = 0;

    // =========================================================================
    // Callbacks
    // =========================================================================

    /**
     * @brief Set callback for network change events.
     *
     * Called when network connectivity changes (type or connected state).
     * Detection occurs within 2 seconds per Requirement 11.1.
     *
     * @param callback Function to call on network changes
     */
    virtual void setNetworkChangeCallback(NetworkChangeCallback callback) = 0;

    /**
     * @brief Set callback for address changes (Requirement 11.3).
     *
     * Called when the server's IP address changes due to interface change.
     *
     * @param callback Function to call with new address
     */
    virtual void setAddressChangeCallback(AddressChangeCallback callback) = 0;

    /**
     * @brief Set callback for cellular data warnings (Requirement 11.6).
     *
     * Called when switching to cellular data to warn about potential charges.
     *
     * @param callback Function to call for cellular warning
     */
    virtual void setCellularWarningCallback(CellularWarningCallback callback) = 0;

    // =========================================================================
    // Cellular Data Warning (Requirement 11.6)
    // =========================================================================

    /**
     * @brief Enable or disable cellular data usage warnings.
     *
     * When enabled, users are warned when the connection switches to
     * cellular data.
     *
     * @param enabled true to enable warnings
     */
    virtual void enableCellularDataWarning(bool enabled) = 0;

    /**
     * @brief Check if cellular data warnings are enabled.
     *
     * @return true if warnings are enabled
     */
    virtual bool isCellularDataWarningEnabled() const = 0;

    /**
     * @brief Check if a data usage warning has been shown.
     *
     * @return true if warning was shown since last reset
     */
    virtual bool wasDataUsageWarningShown() const = 0;

    /**
     * @brief Reset the data usage warning state.
     *
     * Clears the flag so warnings can be shown again.
     */
    virtual void resetDataUsageWarning() = 0;

    // =========================================================================
    // Server Address (Requirement 11.3)
    // =========================================================================

    /**
     * @brief Get the current server address.
     *
     * Returns the IP address the server is currently bound to.
     *
     * @return Current address, or empty if not bound
     */
    virtual std::optional<std::string> getServerAddress() const = 0;

    /**
     * @brief Check if address change was notified.
     *
     * @return true if address change notification was sent
     */
    virtual bool wasAddressChangeNotified() const = 0;

    // =========================================================================
    // Detection Latency (Requirement 11.1)
    // =========================================================================

    /**
     * @brief Get the last network change detection latency.
     *
     * Returns how long it took to detect the last network change.
     * Should be within 2 seconds per Requirement 11.1.
     *
     * @return Detection latency in milliseconds
     */
    virtual std::chrono::milliseconds getLastDetectionLatency() const = 0;
};

// =============================================================================
// Platform-Specific Implementation (iOS)
// =============================================================================

#if defined(__APPLE__) || defined(OPENRTMP_IOS_TEST)

/**
 * @brief iOS-specific network monitor implementation.
 *
 * Uses iOS platform APIs for network monitoring:
 * - NWPathMonitor for path changes
 * - Network.framework for connectivity
 * - SystemConfiguration for network info
 *
 * ## Thread Safety
 * All methods are thread-safe. Callbacks are invoked on a dedicated
 * monitoring queue.
 */
class iOSNetworkMonitor : public INetworkMonitor {
public:
    iOSNetworkMonitor();
    ~iOSNetworkMonitor() override;

    // Non-copyable, non-movable
    iOSNetworkMonitor(const iOSNetworkMonitor&) = delete;
    iOSNetworkMonitor& operator=(const iOSNetworkMonitor&) = delete;

    // Lifecycle
    core::Result<void, NetworkMonitorError> startMonitoring() override;
    void stopMonitoring() override;
    bool isMonitoringActive() const override;

    // Network State
    NetworkState getCurrentState() const override;
    NetworkType getActiveNetworkType() const override;
    bool isConnected() const override;

    // Network Configuration
    void setAllowedNetworks(NetworkTypeFlags allowed) override;
    NetworkTypeFlags getAllowedNetworks() const override;
    bool isNetworkTypeAllowed(NetworkType type) const override;

    // Connection State Preservation
    void setConnectionStateGracePeriod(std::chrono::milliseconds duration) override;
    std::chrono::milliseconds getConnectionStateGracePeriod() const override;
    bool isInGracePeriod() const override;
    std::chrono::milliseconds getRemainingGracePeriod() const override;

    // Interface Rebinding
    core::Result<void, NetworkMonitorError> rebindToInterface(NetworkType type) override;
    bool attemptAutomaticRebind() override;

    // Callbacks
    void setNetworkChangeCallback(NetworkChangeCallback callback) override;
    void setAddressChangeCallback(AddressChangeCallback callback) override;
    void setCellularWarningCallback(CellularWarningCallback callback) override;

    // Cellular Data Warning
    void enableCellularDataWarning(bool enabled) override;
    bool isCellularDataWarningEnabled() const override;
    bool wasDataUsageWarningShown() const override;
    void resetDataUsageWarning() override;

    // Server Address
    std::optional<std::string> getServerAddress() const override;
    bool wasAddressChangeNotified() const override;

    // Detection Latency
    std::chrono::milliseconds getLastDetectionLatency() const override;

private:
    void updateNetworkState();
    std::string getInterfaceAddress(const std::string& interfaceName);

    // State
    std::atomic<bool> isMonitoring_;
    std::atomic<NetworkType> currentNetworkType_;
    std::atomic<bool> isConnected_;
    std::string currentInterfaceName_;
    std::optional<std::string> currentIpAddress_;
    mutable std::mutex stateMutex_;

    // Configuration
    std::atomic<uint32_t> allowedNetworks_;
    std::atomic<uint32_t> gracePeriodMs_;
    std::chrono::steady_clock::time_point disconnectionTime_;

    // Warning state
    std::atomic<bool> cellularWarningEnabled_;
    std::atomic<bool> cellularWarningShown_;
    std::atomic<bool> addressChangeNotified_;

    // Detection latency tracking
    std::atomic<uint32_t> lastDetectionLatencyMs_;
    std::chrono::steady_clock::time_point lastChangeTime_;

    // Callbacks
    NetworkChangeCallback networkChangeCallback_;
    AddressChangeCallback addressChangeCallback_;
    CellularWarningCallback cellularWarningCallback_;
    mutable std::mutex callbackMutex_;

    // Platform-specific handles
    void* pathMonitor_;     // NWPathMonitor
    void* monitorQueue_;    // dispatch_queue_t
};

#endif // __APPLE__

// =============================================================================
// Platform-Specific Implementation (Android)
// =============================================================================

#if defined(__ANDROID__) || defined(OPENRTMP_ANDROID_TEST)

/**
 * @brief Android-specific network monitor implementation.
 *
 * Uses Android platform APIs for network monitoring:
 * - ConnectivityManager for network state
 * - NetworkCallback for connectivity changes
 * - WifiManager for WiFi-specific info
 *
 * ## Thread Safety
 * All methods are thread-safe. JNI calls are minimized for efficiency.
 */
class AndroidNetworkMonitor : public INetworkMonitor {
public:
    AndroidNetworkMonitor();
    ~AndroidNetworkMonitor() override;

    // Non-copyable, non-movable
    AndroidNetworkMonitor(const AndroidNetworkMonitor&) = delete;
    AndroidNetworkMonitor& operator=(const AndroidNetworkMonitor&) = delete;

    /**
     * @brief Initialize with JNI environment.
     *
     * @param javaVM Pointer to JavaVM
     * @param applicationContext Global reference to Application Context
     */
    void initializeJNI(void* javaVM, void* applicationContext);

    // Lifecycle
    core::Result<void, NetworkMonitorError> startMonitoring() override;
    void stopMonitoring() override;
    bool isMonitoringActive() const override;

    // Network State
    NetworkState getCurrentState() const override;
    NetworkType getActiveNetworkType() const override;
    bool isConnected() const override;

    // Network Configuration
    void setAllowedNetworks(NetworkTypeFlags allowed) override;
    NetworkTypeFlags getAllowedNetworks() const override;
    bool isNetworkTypeAllowed(NetworkType type) const override;

    // Connection State Preservation
    void setConnectionStateGracePeriod(std::chrono::milliseconds duration) override;
    std::chrono::milliseconds getConnectionStateGracePeriod() const override;
    bool isInGracePeriod() const override;
    std::chrono::milliseconds getRemainingGracePeriod() const override;

    // Interface Rebinding
    core::Result<void, NetworkMonitorError> rebindToInterface(NetworkType type) override;
    bool attemptAutomaticRebind() override;

    // Callbacks
    void setNetworkChangeCallback(NetworkChangeCallback callback) override;
    void setAddressChangeCallback(AddressChangeCallback callback) override;
    void setCellularWarningCallback(CellularWarningCallback callback) override;

    // Cellular Data Warning
    void enableCellularDataWarning(bool enabled) override;
    bool isCellularDataWarningEnabled() const override;
    bool wasDataUsageWarningShown() const override;
    void resetDataUsageWarning() override;

    // Server Address
    std::optional<std::string> getServerAddress() const override;
    bool wasAddressChangeNotified() const override;

    // Detection Latency
    std::chrono::milliseconds getLastDetectionLatency() const override;

private:
    void onNetworkCallback(bool isConnected, int networkType);
    std::string getInterfaceAddress(const std::string& interfaceName);

    // State
    std::atomic<bool> isMonitoring_;
    std::atomic<NetworkType> currentNetworkType_;
    std::atomic<bool> isConnected_;
    std::string currentInterfaceName_;
    std::optional<std::string> currentIpAddress_;
    mutable std::mutex stateMutex_;

    // Configuration
    std::atomic<uint32_t> allowedNetworks_;
    std::atomic<uint32_t> gracePeriodMs_;
    std::chrono::steady_clock::time_point disconnectionTime_;

    // Warning state
    std::atomic<bool> cellularWarningEnabled_;
    std::atomic<bool> cellularWarningShown_;
    std::atomic<bool> addressChangeNotified_;

    // Detection latency tracking
    std::atomic<uint32_t> lastDetectionLatencyMs_;
    std::chrono::steady_clock::time_point lastChangeTime_;

    // Callbacks
    NetworkChangeCallback networkChangeCallback_;
    AddressChangeCallback addressChangeCallback_;
    CellularWarningCallback cellularWarningCallback_;
    mutable std::mutex callbackMutex_;

    // JNI handles
    void* javaVM_;              // JavaVM*
    void* applicationContext_;  // jobject global ref
    void* networkCallback_;     // jobject global ref
};

#endif // __ANDROID__

} // namespace mobile
} // namespace pal
} // namespace openrtmp

#endif // OPENRTMP_PAL_MOBILE_NETWORK_MONITOR_HPP
