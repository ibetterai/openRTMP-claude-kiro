// OpenRTMP - Cross-platform RTMP Server
// iOS Background Task Manager Interface
//
// This component provides iOS-specific background execution management:
// - Background task continuation (beginBackgroundTask API wrapper)
// - Background time monitoring with client notifications
// - Audio session management for audio background mode
// - Background App Refresh registration
// - CPU optimization modes for battery efficiency
// - Video-only stream warning system
//
// Requirements Covered: 8.4, 8.6, 9.1, 9.2, 9.5, 9.6

#ifndef OPENRTMP_PAL_IOS_BACKGROUND_TASK_HPP
#define OPENRTMP_PAL_IOS_BACKGROUND_TASK_HPP

#if defined(__APPLE__)

#include "openrtmp/core/result.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>

namespace openrtmp {
namespace pal {
namespace ios {

// =============================================================================
// Enumerations
// =============================================================================

/**
 * @brief Application state for lifecycle management.
 */
enum class AppState {
    Foreground,     ///< App is in the foreground
    Background,     ///< App is in the background
    Suspended       ///< App is suspended
};

/**
 * @brief Stream type classification for background capability determination.
 */
enum class StreamType {
    None,           ///< No active stream
    AudioOnly,      ///< Audio-only stream (can continue with audio background mode)
    VideoOnly,      ///< Video-only stream (requires foreground)
    AudioVideo      ///< Audio and video stream
};

/**
 * @brief Audio session category for iOS.
 *
 * Maps to AVAudioSession categories.
 */
enum class AudioSessionCategory {
    None,           ///< No audio session configured
    Ambient,        ///< AVAudioSessionCategoryAmbient
    SoloAmbient,    ///< AVAudioSessionCategorySoloAmbient
    Playback,       ///< AVAudioSessionCategoryPlayback
    Record,         ///< AVAudioSessionCategoryRecord
    PlayAndRecord,  ///< AVAudioSessionCategoryPlayAndRecord
    MultiRoute      ///< AVAudioSessionCategoryMultiRoute
};

/**
 * @brief CPU usage mode for power optimization.
 */
enum class CPUUsageMode {
    Normal,         ///< Full CPU usage allowed
    Reduced,        ///< Reduced non-essential processing
    Minimal         ///< Minimum CPU usage for critical operations only
};

/**
 * @brief Background task error codes.
 */
enum class BackgroundTaskErrorCode {
    Success = 0,
    Unknown = 1,
    InvalidArgument = 100,
    TaskNotStarted = 101,
    TaskExpired = 102,
    AudioSessionError = 200,
    AudioSessionNotConfigured = 201,
    AudioSessionActivationFailed = 202,
    BackgroundRefreshUnavailable = 300,
    BackgroundRefreshDenied = 301
};

// =============================================================================
// Data Structures
// =============================================================================

/**
 * @brief Background task error information.
 */
struct BackgroundTaskError {
    BackgroundTaskErrorCode code;
    std::string message;

    BackgroundTaskError(BackgroundTaskErrorCode c = BackgroundTaskErrorCode::Unknown,
                        std::string msg = "")
        : code(c), message(std::move(msg)) {}
};

// =============================================================================
// Callback Types
// =============================================================================

/**
 * @brief Callback invoked when background task is about to expire.
 *
 * The callback should perform cleanup and notify clients of impending
 * disconnection within the remaining time (typically ~5 seconds).
 */
using ExpirationCallback = std::function<void()>;

/**
 * @brief Callback invoked periodically with remaining background time.
 *
 * @param remainingTime The remaining background execution time
 */
using TimeMonitorCallback = std::function<void(std::chrono::seconds remainingTime)>;

/**
 * @brief Callback for warning messages.
 *
 * @param message Warning message to display to user
 */
using WarningCallback = std::function<void(const std::string& message)>;

// =============================================================================
// Interface Definition
// =============================================================================

/**
 * @brief Abstract interface for iOS background task management.
 *
 * This interface defines the contract for iOS-specific background execution
 * features. Implementations wrap iOS APIs (UIApplication, AVAudioSession,
 * BGTaskScheduler) to provide background execution capabilities for the
 * RTMP server.
 *
 * ## Background Task Continuation (Requirement 9.1)
 * When the app enters background, request extended execution time using
 * beginBackgroundTask(withName:expirationHandler:). This provides approximately
 * 3 minutes of background execution time.
 *
 * ## Audio Background Mode (Requirement 9.1)
 * For audio-only streams, configure the audio session with the .playback
 * category and enable the "Audio, AirPlay, and Picture in Picture" background
 * mode. This allows indefinite background execution for audio streaming.
 *
 * ## CPU Optimization (Requirement 9.2)
 * In background mode, reduce CPU usage by disabling non-essential processing
 * such as statistics updates, debug logging, and UI updates.
 *
 * ## Background App Refresh (Requirement 9.6)
 * Register for periodic background refresh using BGTaskScheduler to check
 * server state and perform maintenance when the app is not active.
 */
class IBackgroundTaskManager {
public:
    virtual ~IBackgroundTaskManager() = default;

    // =========================================================================
    // Background Task Lifecycle (Requirement 9.1)
    // =========================================================================

    /**
     * @brief Begin a background task with expiration handler.
     *
     * Requests additional background execution time from iOS. The task
     * typically provides ~3 minutes of execution time before the expiration
     * handler is called.
     *
     * @param taskName Descriptive name for the task (for debugging)
     * @param callback Handler called when task is about to expire
     * @return Success or error
     */
    virtual core::Result<void, BackgroundTaskError> beginBackgroundTask(
        const std::string& taskName,
        ExpirationCallback callback) = 0;

    /**
     * @brief End the current background task.
     *
     * Call this when background work is complete or the app returns
     * to foreground. Failing to end the task will result in app termination.
     */
    virtual void endBackgroundTask() = 0;

    /**
     * @brief Check if a background task is currently active.
     *
     * @return true if background task is running
     */
    virtual bool isBackgroundTaskActive() const = 0;

    // =========================================================================
    // Background Time Monitoring (Requirement 9.5)
    // =========================================================================

    /**
     * @brief Get the remaining background execution time.
     *
     * Returns the approximate time remaining before the background task
     * expires. Returns 0 if no background task is active.
     *
     * @return Remaining time in seconds
     */
    virtual std::chrono::seconds getRemainingBackgroundTime() const = 0;

    /**
     * @brief Set callback for background time monitoring.
     *
     * The callback is invoked periodically (typically every 10 seconds)
     * with the remaining background time, allowing clients to prepare
     * for disconnection.
     *
     * @param callback Handler called with remaining time
     */
    virtual void setBackgroundTimeMonitorCallback(TimeMonitorCallback callback) = 0;

    // =========================================================================
    // Background App Refresh (Requirement 9.6)
    // =========================================================================

    /**
     * @brief Register for Background App Refresh.
     *
     * Registers a BGAppRefreshTask with the system to periodically
     * check server state in the background.
     *
     * @param identifier Task identifier (must match Info.plist registration)
     * @return Success or error
     */
    virtual core::Result<void, BackgroundTaskError> registerForBackgroundAppRefresh(
        const std::string& identifier) = 0;

    /**
     * @brief Check if Background App Refresh is registered.
     *
     * @return true if registered
     */
    virtual bool isBackgroundAppRefreshRegistered() const = 0;

    // =========================================================================
    // Audio Session Management (Requirement 9.1 - Audio Background Mode)
    // =========================================================================

    /**
     * @brief Configure the audio session for background audio.
     *
     * Sets up the AVAudioSession with the specified category.
     * For background audio streaming, use Playback or PlayAndRecord.
     *
     * @param category Audio session category
     * @return Success or error
     */
    virtual core::Result<void, BackgroundTaskError> configureAudioSession(
        AudioSessionCategory category) = 0;

    /**
     * @brief Activate the audio session.
     *
     * Must be called before audio can play in background.
     * Requires prior configuration with configureAudioSession().
     *
     * @return Success or error
     */
    virtual core::Result<void, BackgroundTaskError> activateAudioSession() = 0;

    /**
     * @brief Deactivate the audio session.
     *
     * Call when audio streaming is complete.
     *
     * @return Success or error
     */
    virtual core::Result<void, BackgroundTaskError> deactivateAudioSession() = 0;

    /**
     * @brief Check if audio session is currently active.
     *
     * @return true if audio session is active
     */
    virtual bool isAudioSessionActive() const = 0;

    /**
     * @brief Get the current audio session category.
     *
     * @return Current audio session category
     */
    virtual AudioSessionCategory getAudioSessionCategory() const = 0;

    // =========================================================================
    // Stream Type Management
    // =========================================================================

    /**
     * @brief Set the current stream type.
     *
     * This affects background capability:
     * - AudioOnly: Can continue indefinitely with audio background mode
     * - VideoOnly: Requires foreground; limited to ~3 minutes in background
     * - AudioVideo: Audio may continue; video will be affected
     *
     * @param type Stream type
     */
    virtual void setCurrentStreamType(StreamType type) = 0;

    /**
     * @brief Get the current stream type.
     *
     * @return Current stream type
     */
    virtual StreamType getCurrentStreamType() const = 0;

    /**
     * @brief Check if the current stream can continue in background.
     *
     * Returns true if:
     * - Stream is audio-only AND audio session is active, OR
     * - Background task is active (limited time)
     *
     * @return true if streaming can continue
     */
    virtual bool canContinueInBackground() const = 0;

    // =========================================================================
    // App State Management
    // =========================================================================

    /**
     * @brief Notify manager that app is entering background.
     *
     * Call from applicationWillResignActive or similar lifecycle method.
     * Automatically starts background task and reduces CPU usage.
     */
    virtual void onAppWillEnterBackground() = 0;

    /**
     * @brief Notify manager that app is entering foreground.
     *
     * Call from applicationDidBecomeActive or similar lifecycle method.
     * Ends background task and restores normal CPU usage.
     */
    virtual void onAppWillEnterForeground() = 0;

    /**
     * @brief Get current app state.
     *
     * @return Current app state
     */
    virtual AppState getAppState() const = 0;

    // =========================================================================
    // CPU Usage Optimization (Requirement 9.2)
    // =========================================================================

    /**
     * @brief Set CPU usage mode.
     *
     * In background, use Reduced or Minimal to conserve battery.
     * This should trigger reduction of non-essential processing.
     *
     * @param mode CPU usage mode
     */
    virtual void setCPUUsageMode(CPUUsageMode mode) = 0;

    /**
     * @brief Get current CPU usage mode.
     *
     * @return Current CPU usage mode
     */
    virtual CPUUsageMode getCPUUsageMode() const = 0;

    // =========================================================================
    // Warning System
    // =========================================================================

    /**
     * @brief Set callback for video-only stream background warning.
     *
     * When a video-only stream is active and app enters background,
     * this callback is invoked to warn the user that streaming
     * requires foreground.
     *
     * @param callback Warning callback
     */
    virtual void setVideoOnlyBackgroundWarningCallback(WarningCallback callback) = 0;

    /**
     * @brief Check if video-only warning should be shown.
     *
     * Returns true if:
     * - App is in background AND
     * - Stream type is VideoOnly
     *
     * @return true if warning should be shown
     */
    virtual bool shouldShowVideoOnlyWarning() const = 0;
};

// =============================================================================
// iOS Implementation
// =============================================================================

/**
 * @brief iOS-specific background task manager implementation.
 *
 * Wraps iOS APIs for background execution:
 * - UIApplication.shared.beginBackgroundTask for extended execution
 * - AVAudioSession for audio background mode
 * - BGTaskScheduler for Background App Refresh
 *
 * ## Info.plist Requirements
 * For audio background mode, add to Info.plist:
 * ```xml
 * <key>UIBackgroundModes</key>
 * <array>
 *     <string>audio</string>
 *     <string>fetch</string>
 * </array>
 * ```
 *
 * For Background App Refresh:
 * ```xml
 * <key>BGTaskSchedulerPermittedIdentifiers</key>
 * <array>
 *     <string>com.openrtmp.refresh</string>
 * </array>
 * ```
 *
 * ## Thread Safety
 * All methods are thread-safe. Callbacks are invoked on the main thread.
 */
class iOSBackgroundTaskManager : public IBackgroundTaskManager {
public:
    iOSBackgroundTaskManager();
    ~iOSBackgroundTaskManager() override;

    // Non-copyable, non-movable
    iOSBackgroundTaskManager(const iOSBackgroundTaskManager&) = delete;
    iOSBackgroundTaskManager& operator=(const iOSBackgroundTaskManager&) = delete;

    // Background Task Lifecycle
    core::Result<void, BackgroundTaskError> beginBackgroundTask(
        const std::string& taskName,
        ExpirationCallback callback) override;
    void endBackgroundTask() override;
    bool isBackgroundTaskActive() const override;

    // Background Time Monitoring
    std::chrono::seconds getRemainingBackgroundTime() const override;
    void setBackgroundTimeMonitorCallback(TimeMonitorCallback callback) override;

    // Background App Refresh
    core::Result<void, BackgroundTaskError> registerForBackgroundAppRefresh(
        const std::string& identifier) override;
    bool isBackgroundAppRefreshRegistered() const override;

    // Audio Session Management
    core::Result<void, BackgroundTaskError> configureAudioSession(
        AudioSessionCategory category) override;
    core::Result<void, BackgroundTaskError> activateAudioSession() override;
    core::Result<void, BackgroundTaskError> deactivateAudioSession() override;
    bool isAudioSessionActive() const override;
    AudioSessionCategory getAudioSessionCategory() const override;

    // Stream Type Management
    void setCurrentStreamType(StreamType type) override;
    StreamType getCurrentStreamType() const override;
    bool canContinueInBackground() const override;

    // App State Management
    void onAppWillEnterBackground() override;
    void onAppWillEnterForeground() override;
    AppState getAppState() const override;

    // CPU Usage Optimization
    void setCPUUsageMode(CPUUsageMode mode) override;
    CPUUsageMode getCPUUsageMode() const override;

    // Warning System
    void setVideoOnlyBackgroundWarningCallback(WarningCallback callback) override;
    bool shouldShowVideoOnlyWarning() const override;

private:
    // Start background time monitoring timer
    void startTimeMonitoring();
    void stopTimeMonitoring();

    // Internal state
    std::atomic<AppState> appState_;
    std::atomic<StreamType> currentStreamType_;
    std::atomic<CPUUsageMode> cpuUsageMode_;
    std::atomic<AudioSessionCategory> audioSessionCategory_;
    std::atomic<bool> isAudioSessionActive_;
    std::atomic<bool> isBackgroundTaskActive_;
    std::atomic<bool> isBackgroundAppRefreshRegistered_;

    // Callbacks
    ExpirationCallback expirationCallback_;
    TimeMonitorCallback timeMonitorCallback_;
    WarningCallback warningCallback_;
    std::mutex callbackMutex_;

    // Platform-specific handles (opaque to avoid Objective-C in header)
    // In actual implementation, these would be Objective-C object handles
    void* backgroundTaskIdentifier_;  // UIBackgroundTaskIdentifier
    void* timeMonitoringTimer_;       // Timer for monitoring remaining time
};

} // namespace ios
} // namespace pal
} // namespace openrtmp

#endif // __APPLE__

#endif // OPENRTMP_PAL_IOS_BACKGROUND_TASK_HPP
