// OpenRTMP - Cross-platform RTMP Server
// Android Foreground Service Interface
//
// This component provides Android-specific foreground service management:
// - Foreground service lifecycle (start/stop on app background transitions)
// - Notification channel and content management
// - Power conservation mode detection (Doze mode, App Standby)
// - Permission management for INTERNET and ACCESS_NETWORK_STATE
//
// Requirements Covered: 8.5, 8.6, 9.3, 9.4, 10.6

#ifndef OPENRTMP_PAL_ANDROID_FOREGROUND_SERVICE_HPP
#define OPENRTMP_PAL_ANDROID_FOREGROUND_SERVICE_HPP

#if defined(__ANDROID__) || defined(OPENRTMP_ANDROID_TEST)

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
namespace android {

// =============================================================================
// Enumerations
// =============================================================================

/**
 * @brief Application state for lifecycle management.
 */
enum class AppState {
    Foreground,     ///< App is in the foreground
    Background,     ///< App is in the background
    Stopped         ///< App is stopped
};

/**
 * @brief Power conservation mode.
 */
enum class PowerConservationMode {
    Normal,         ///< Normal operation, no power restrictions
    AppStandby,     ///< App Standby bucket restrictions
    Doze            ///< Doze mode (deep sleep) restrictions
};

/**
 * @brief Notification importance level.
 *
 * Maps to Android NotificationManager.IMPORTANCE_* constants.
 */
enum class NotificationImportance {
    None = 0,       ///< IMPORTANCE_NONE - No notifications shown
    Min = 1,        ///< IMPORTANCE_MIN - No sound, no visual presence
    Low = 2,        ///< IMPORTANCE_LOW - No sound, shows in shade
    Default = 3,    ///< IMPORTANCE_DEFAULT - Sound, shows in shade
    High = 4,       ///< IMPORTANCE_HIGH - Sound, peek
    Max = 5         ///< IMPORTANCE_MAX - Sound, peek, full screen
};

/**
 * @brief Android permissions required by OpenRTMP.
 */
enum class AndroidPermission {
    Internet,               ///< android.permission.INTERNET
    AccessNetworkState,     ///< android.permission.ACCESS_NETWORK_STATE
    ForegroundService,      ///< android.permission.FOREGROUND_SERVICE (API 28+)
    PostNotifications,      ///< android.permission.POST_NOTIFICATIONS (API 33+)
    WakeLock                ///< android.permission.WAKE_LOCK
};

/**
 * @brief Foreground service error codes.
 */
enum class ForegroundServiceErrorCode {
    Success = 0,
    Unknown = 1,
    InvalidConfiguration = 100,
    ServiceNotRunning = 101,
    PermissionDenied = 102,
    NotificationError = 200,
    ChannelCreationFailed = 201,
    JNIError = 300
};

// =============================================================================
// Data Structures
// =============================================================================

/**
 * @brief Foreground service error information.
 */
struct ForegroundServiceError {
    ForegroundServiceErrorCode code;
    std::string message;

    ForegroundServiceError(ForegroundServiceErrorCode c = ForegroundServiceErrorCode::Unknown,
                           std::string msg = "")
        : code(c), message(std::move(msg)) {}
};

/**
 * @brief Configuration for starting the foreground service.
 */
struct ForegroundServiceConfig {
    std::string channelId;          ///< Notification channel ID
    int notificationId = 0;         ///< Unique notification ID
    std::string title;              ///< Notification title
    std::string content;            ///< Notification content text
    int smallIconResource = 0;      ///< Resource ID for small icon
    bool ongoing = true;            ///< Whether notification is ongoing
    bool showWhen = false;          ///< Whether to show timestamp
};

/**
 * @brief Configuration for notification channel creation.
 */
struct NotificationChannelConfig {
    std::string channelId;          ///< Unique channel identifier
    std::string channelName;        ///< User-visible name
    std::string description;        ///< User-visible description
    NotificationImportance importance = NotificationImportance::Low;
    bool enableVibration = false;   ///< Enable vibration
    bool enableLights = false;      ///< Enable notification lights
    bool showBadge = false;         ///< Show app badge
};

/**
 * @brief Update for notification content.
 */
struct NotificationUpdate {
    std::string title;              ///< Updated title (empty = no change)
    std::string content;            ///< Updated content (empty = no change)
    uint32_t activeStreamCount = 0; ///< Number of active streams
    uint32_t connectionCount = 0;   ///< Number of connections
};

// =============================================================================
// Callback Types
// =============================================================================

/**
 * @brief Callback invoked when power conservation mode changes.
 *
 * @param mode The new power conservation mode
 */
using PowerConservationCallback = std::function<void(PowerConservationMode mode)>;

/**
 * @brief Callback invoked when service state changes.
 *
 * @param isRunning true if service is now running
 */
using ServiceStateCallback = std::function<void(bool isRunning)>;

// =============================================================================
// Interface Definition
// =============================================================================

/**
 * @brief Abstract interface for Android foreground service management.
 *
 * This interface defines the contract for Android-specific background execution
 * features. Implementations wrap Android APIs via JNI (Context, Service,
 * NotificationManager, PowerManager) to provide foreground service capabilities
 * for the RTMP server.
 *
 * ## Foreground Service (Requirement 9.3)
 * Android requires a foreground service with persistent notification for any
 * long-running background operation. This prevents the system from killing
 * the app when it's in the background.
 *
 * ## Notification (Requirement 9.4)
 * The notification must display server status including active stream count
 * and connection count. Users can tap the notification to return to the app.
 *
 * ## Power Conservation (Requirement 10.6)
 * The service must detect and respect Doze mode and App Standby:
 * - Doze mode: Device is idle, stationary, screen off. Network access restricted.
 * - App Standby: App not used recently. Background work restricted.
 *
 * ## Required Permissions (Requirement 8.5)
 * AndroidManifest.xml must declare:
 * ```xml
 * <uses-permission android:name="android.permission.INTERNET" />
 * <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
 * <uses-permission android:name="android.permission.FOREGROUND_SERVICE" />
 * <uses-permission android:name="android.permission.POST_NOTIFICATIONS" />
 * ```
 */
class IAndroidForegroundService {
public:
    virtual ~IAndroidForegroundService() = default;

    // =========================================================================
    // Foreground Service Lifecycle (Requirement 9.3)
    // =========================================================================

    /**
     * @brief Start the foreground service with notification.
     *
     * Must be called within 5 seconds of receiving a startForegroundService()
     * intent, or the system will throw an exception.
     *
     * @param config Service configuration including notification settings
     * @return Success or error
     */
    virtual core::Result<void, ForegroundServiceError> startForegroundService(
        const ForegroundServiceConfig& config) = 0;

    /**
     * @brief Stop the foreground service.
     *
     * Should be called when the app returns to foreground or when
     * background operation is no longer needed.
     */
    virtual void stopForegroundService() = 0;

    /**
     * @brief Check if the foreground service is currently running.
     *
     * @return true if service is running
     */
    virtual bool isServiceRunning() const = 0;

    // =========================================================================
    // Notification Management (Requirement 9.4)
    // =========================================================================

    /**
     * @brief Create a notification channel.
     *
     * Required on Android 8.0 (API 26) and higher before showing notifications.
     * Should be called during app initialization.
     *
     * @param config Channel configuration
     * @return Success or error
     */
    virtual core::Result<void, ForegroundServiceError> createNotificationChannel(
        const NotificationChannelConfig& config) = 0;

    /**
     * @brief Update the foreground service notification.
     *
     * Updates the notification content to reflect current server state.
     * Should be called when stream or connection counts change.
     *
     * @param update Notification update with new content
     * @return Success or error
     */
    virtual core::Result<void, ForegroundServiceError> updateNotification(
        const NotificationUpdate& update) = 0;

    /**
     * @brief Get the current notification title.
     *
     * @return Current notification title
     */
    virtual std::string getNotificationTitle() const = 0;

    /**
     * @brief Get the current notification content.
     *
     * @return Current notification content
     */
    virtual std::string getNotificationContent() const = 0;

    /**
     * @brief Get the active stream count displayed in notification.
     *
     * @return Active stream count
     */
    virtual uint32_t getActiveStreamCount() const = 0;

    /**
     * @brief Get the connection count displayed in notification.
     *
     * @return Connection count
     */
    virtual uint32_t getConnectionCount() const = 0;

    // =========================================================================
    // Power Conservation (Requirement 10.6)
    // =========================================================================

    /**
     * @brief Check if the device is in Doze mode.
     *
     * Doze mode restricts network access, CPU wake-ups, and alarms.
     * The app should reduce activity when in Doze mode.
     *
     * @return true if in Doze mode
     */
    virtual bool isInDozeMode() const = 0;

    /**
     * @brief Check if the app is in App Standby.
     *
     * App Standby restricts background work for apps not recently used.
     *
     * @return true if in App Standby
     */
    virtual bool isInAppStandby() const = 0;

    /**
     * @brief Get the current power conservation mode.
     *
     * @return Current power conservation mode
     */
    virtual PowerConservationMode getPowerConservationMode() const = 0;

    /**
     * @brief Set callback for power conservation mode changes.
     *
     * The callback is invoked when entering or exiting Doze mode
     * or App Standby.
     *
     * @param callback Handler called on mode change
     */
    virtual void setPowerConservationCallback(PowerConservationCallback callback) = 0;

    // =========================================================================
    // App Lifecycle
    // =========================================================================

    /**
     * @brief Notify manager that app is entering background.
     *
     * Call from Activity.onStop() or similar lifecycle method.
     * Automatically starts foreground service if not already running.
     */
    virtual void onAppWillEnterBackground() = 0;

    /**
     * @brief Notify manager that app is entering foreground.
     *
     * Call from Activity.onStart() or similar lifecycle method.
     * Stops foreground service if running.
     */
    virtual void onAppWillEnterForeground() = 0;

    /**
     * @brief Get current app state.
     *
     * @return Current app state
     */
    virtual AppState getAppState() const = 0;

    // =========================================================================
    // Permission Management (Requirement 8.5)
    // =========================================================================

    /**
     * @brief Check if a specific permission is granted.
     *
     * @param permission The permission to check
     * @return true if permission is granted
     */
    virtual bool hasPermission(AndroidPermission permission) const = 0;

    /**
     * @brief Get list of required but missing permissions.
     *
     * @return Vector of missing permissions
     */
    virtual std::vector<AndroidPermission> getMissingPermissions() const = 0;

    /**
     * @brief Request a runtime permission.
     *
     * This triggers the Android permission request dialog.
     * Note: INTERNET and ACCESS_NETWORK_STATE are normal permissions
     * and don't require runtime request.
     *
     * @param permission The permission to request
     * @return Success or error (denied/not granted)
     */
    virtual core::Result<void, ForegroundServiceError> requestPermission(
        AndroidPermission permission) = 0;
};

// =============================================================================
// Android Implementation
// =============================================================================

/**
 * @brief Android-specific foreground service implementation.
 *
 * Wraps Android APIs via JNI for background execution:
 * - Context.startForegroundService() for starting service
 * - Service.startForeground() for showing notification
 * - NotificationManager for notification management
 * - PowerManager for Doze mode detection
 *
 * ## AndroidManifest.xml Requirements
 * ```xml
 * <!-- Required permissions -->
 * <uses-permission android:name="android.permission.INTERNET" />
 * <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
 * <uses-permission android:name="android.permission.FOREGROUND_SERVICE" />
 * <uses-permission android:name="android.permission.POST_NOTIFICATIONS" />
 *
 * <!-- Foreground service declaration -->
 * <service
 *     android:name=".OpenRTMPForegroundService"
 *     android:foregroundServiceType="dataSync"
 *     android:exported="false" />
 * ```
 *
 * ## Thread Safety
 * All methods are thread-safe. JNI calls are marshalled to the main thread.
 */
class AndroidForegroundService : public IAndroidForegroundService {
public:
    AndroidForegroundService();
    ~AndroidForegroundService() override;

    // Non-copyable, non-movable
    AndroidForegroundService(const AndroidForegroundService&) = delete;
    AndroidForegroundService& operator=(const AndroidForegroundService&) = delete;

    /**
     * @brief Initialize with JNI environment.
     *
     * Must be called from JNI_OnLoad or before any other methods.
     *
     * @param javaVM Pointer to JavaVM
     * @param applicationContext Global reference to Application Context
     */
    void initializeJNI(void* javaVM, void* applicationContext);

    // Foreground Service Lifecycle
    core::Result<void, ForegroundServiceError> startForegroundService(
        const ForegroundServiceConfig& config) override;
    void stopForegroundService() override;
    bool isServiceRunning() const override;

    // Notification Management
    core::Result<void, ForegroundServiceError> createNotificationChannel(
        const NotificationChannelConfig& config) override;
    core::Result<void, ForegroundServiceError> updateNotification(
        const NotificationUpdate& update) override;
    std::string getNotificationTitle() const override;
    std::string getNotificationContent() const override;
    uint32_t getActiveStreamCount() const override;
    uint32_t getConnectionCount() const override;

    // Power Conservation
    bool isInDozeMode() const override;
    bool isInAppStandby() const override;
    PowerConservationMode getPowerConservationMode() const override;
    void setPowerConservationCallback(PowerConservationCallback callback) override;

    // App Lifecycle
    void onAppWillEnterBackground() override;
    void onAppWillEnterForeground() override;
    AppState getAppState() const override;

    // Permission Management
    bool hasPermission(AndroidPermission permission) const override;
    std::vector<AndroidPermission> getMissingPermissions() const override;
    core::Result<void, ForegroundServiceError> requestPermission(
        AndroidPermission permission) override;

private:
    // JNI helpers
    bool isJNIInitialized() const;
    void registerPowerReceiver();
    void unregisterPowerReceiver();

    // Internal state
    std::atomic<AppState> appState_;
    std::atomic<bool> isServiceRunning_;
    std::atomic<bool> isDozeMode_;
    std::atomic<bool> isAppStandby_;
    std::atomic<PowerConservationMode> powerConservationMode_;
    std::atomic<uint32_t> activeStreamCount_;
    std::atomic<uint32_t> connectionCount_;
    std::string notificationTitle_;
    std::string notificationContent_;
    mutable std::mutex stateMutex_;

    // Callbacks
    PowerConservationCallback powerConservationCallback_;
    std::mutex callbackMutex_;

    // Configuration
    ForegroundServiceConfig currentConfig_;
    NotificationChannelConfig currentChannelConfig_;

    // JNI handles (opaque to avoid JNI in header)
    void* javaVM_;              // JavaVM*
    void* applicationContext_;  // jobject global ref
    void* powerReceiver_;       // BroadcastReceiver for power events
};

/**
 * @brief Get the string name of an Android permission.
 *
 * @param permission The permission enum value
 * @return Permission string (e.g., "android.permission.INTERNET")
 */
inline std::string getPermissionString(AndroidPermission permission) {
    switch (permission) {
        case AndroidPermission::Internet:
            return "android.permission.INTERNET";
        case AndroidPermission::AccessNetworkState:
            return "android.permission.ACCESS_NETWORK_STATE";
        case AndroidPermission::ForegroundService:
            return "android.permission.FOREGROUND_SERVICE";
        case AndroidPermission::PostNotifications:
            return "android.permission.POST_NOTIFICATIONS";
        case AndroidPermission::WakeLock:
            return "android.permission.WAKE_LOCK";
        default:
            return "";
    }
}

} // namespace android
} // namespace pal
} // namespace openrtmp

#endif // __ANDROID__ || OPENRTMP_ANDROID_TEST

#endif // OPENRTMP_PAL_ANDROID_FOREGROUND_SERVICE_HPP
