// OpenRTMP - Cross-platform RTMP Server
// Mobile Battery Optimization Interface
//
// This component provides battery optimization for mobile platforms:
// - Low-power idle mode with <1 wake-up per second when no streams active
// - Platform-native async I/O to avoid busy-wait loops
// - Network operation batching to reduce radio wake-ups
// - Battery usage statistics through API
// - App Store compliance for background execution
//
// Requirements Covered: 10.1, 10.2, 10.3, 10.4, 10.5

#ifndef OPENRTMP_PAL_MOBILE_BATTERY_OPTIMIZATION_HPP
#define OPENRTMP_PAL_MOBILE_BATTERY_OPTIMIZATION_HPP

#include "openrtmp/core/result.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace openrtmp {
namespace pal {
namespace mobile {

// =============================================================================
// Enumerations
// =============================================================================

/**
 * @brief Power mode states for battery optimization.
 *
 * Controls the aggressiveness of power-saving measures.
 */
enum class PowerMode {
    Normal,     ///< Full performance, no power restrictions
    LowPower,   ///< Reduced activity, longer timer intervals
    Idle        ///< Minimum power usage, <1 wake-up per second
};

/**
 * @brief Network operation type for batching.
 */
enum class NetworkOperationType {
    Send,       ///< Outgoing data
    Receive,    ///< Incoming data
    Control     ///< Control/signaling data
};

/**
 * @brief Battery optimization error codes.
 */
enum class BatteryErrorCode {
    Success = 0,
    Unknown = 1,
    InvalidConfiguration = 100,
    InvalidState = 101,
    BatchingFailed = 200,
    FlushFailed = 201,
    StatisticsUnavailable = 300
};

// =============================================================================
// Data Structures
// =============================================================================

/**
 * @brief Battery optimization error information.
 */
struct BatteryError {
    BatteryErrorCode code;
    std::string message;

    BatteryError(BatteryErrorCode c = BatteryErrorCode::Unknown,
                 std::string msg = "")
        : code(c), message(std::move(msg)) {}
};

/**
 * @brief Network operation for batching.
 */
struct NetworkOperation {
    NetworkOperationType type;
    uint32_t dataSize;
    uint32_t priority = 0;      ///< Higher priority = flush sooner
    void* userData = nullptr;    ///< Optional user data
};

/**
 * @brief Battery usage statistics (Requirement 10.4).
 *
 * Provides detailed metrics about battery consumption.
 */
struct BatteryStatistics {
    uint64_t cpuTimeMs = 0;              ///< Total CPU time used in milliseconds
    uint64_t networkBytesTransferred = 0; ///< Total network bytes sent/received
    uint64_t networkOperationsCount = 0;  ///< Total network operations performed
    uint64_t batchedOperationsCount = 0;  ///< Operations that were batched
    uint64_t radioWakeUps = 0;            ///< Number of radio wake-ups
    uint64_t wakeUpCount = 0;             ///< Total wake-up count
    double averageWakeUpFrequencyHz = 0;  ///< Average wake-up frequency
    uint64_t idleTimeMs = 0;              ///< Time spent in idle mode
};

/**
 * @brief App Store compliance status (Requirement 10.5).
 *
 * Indicates whether current battery usage is compliant with
 * App Store guidelines for background execution.
 */
struct ComplianceStatus {
    bool isCompliant = true;             ///< Overall compliance status
    std::vector<std::string> issues;      ///< List of compliance issues
};

// =============================================================================
// Callback Types
// =============================================================================

/**
 * @brief Callback for power mode changes.
 *
 * @param newMode The new power mode
 */
using PowerModeCallback = std::function<void(PowerMode newMode)>;

/**
 * @brief Callback for wake-up events.
 *
 * @param wakeUpCount Current wake-up count
 * @param frequencyHz Current wake-up frequency
 */
using WakeUpCallback = std::function<void(uint64_t wakeUpCount, double frequencyHz)>;

// =============================================================================
// Interface Definition
// =============================================================================

/**
 * @brief Abstract interface for mobile battery optimization.
 *
 * This interface defines the contract for mobile-specific battery optimization
 * features. Implementations provide platform-specific power management for
 * iOS and Android.
 *
 * ## Low-Power Idle Mode (Requirement 10.1)
 * When no streams are active, enter a low-power idle mode that reduces
 * CPU wake-ups to less than 1 per second. This significantly reduces
 * battery drain when the server is running but not actively streaming.
 *
 * ## Platform-Native Async I/O (Requirement 10.2)
 * Use platform-native async I/O (kqueue on iOS, epoll on Android) to avoid
 * busy-wait loops that would drain battery. The implementation should never
 * spin-wait or poll at high frequency.
 *
 * ## Network Operation Batching (Requirement 10.3)
 * Batch network operations to reduce radio wake-ups. Mobile radios consume
 * significant power when transitioning from idle to active state. Batching
 * multiple operations into single radio wake-ups improves battery life.
 *
 * ## Battery Statistics API (Requirement 10.4)
 * Provide detailed battery usage statistics through a programmatic API,
 * including CPU time, network bytes transferred, and wake-up frequency.
 *
 * ## App Store Compliance (Requirement 10.5)
 * Ensure battery usage complies with Apple App Store and Google Play
 * guidelines for background execution.
 */
class IBatteryOptimization {
public:
    virtual ~IBatteryOptimization() = default;

    // =========================================================================
    // Power Mode Management (Requirement 10.1)
    // =========================================================================

    /**
     * @brief Set the current power mode.
     *
     * Changes the power mode which affects timer intervals, wake-up
     * frequency, and processing priority.
     *
     * @param mode The desired power mode
     * @return Success or error
     */
    virtual core::Result<void, BatteryError> setPowerMode(PowerMode mode) = 0;

    /**
     * @brief Get the current power mode.
     *
     * @return Current power mode
     */
    virtual PowerMode getPowerMode() const = 0;

    // =========================================================================
    // Idle Mode Management (Requirement 10.1)
    // =========================================================================

    /**
     * @brief Enter low-power idle mode.
     *
     * Enters a low-power state with <1 wake-up per second. Should be
     * called when no streams are active. This is more aggressive than
     * just setting PowerMode::Idle.
     */
    virtual void enterIdleMode() = 0;

    /**
     * @brief Exit low-power idle mode.
     *
     * Returns to normal operation. Called when streams become active.
     */
    virtual void exitIdleMode() = 0;

    /**
     * @brief Check if currently in idle mode.
     *
     * @return true if in low-power idle mode
     */
    virtual bool isInIdleMode() const = 0;

    // =========================================================================
    // Stream Activity Tracking
    // =========================================================================

    /**
     * @brief Notify that a stream has started.
     *
     * Automatically exits idle mode if currently idle.
     */
    virtual void onStreamStarted() = 0;

    /**
     * @brief Notify that a stream has stopped.
     *
     * Automatically enters idle mode if no streams remain active.
     */
    virtual void onStreamStopped() = 0;

    /**
     * @brief Get the current active stream count.
     *
     * @return Number of active streams
     */
    virtual uint32_t getActiveStreamCount() const = 0;

    // =========================================================================
    // Wake-up Frequency Management (Requirement 10.1)
    // =========================================================================

    /**
     * @brief Record a wake-up event.
     *
     * Call this whenever the server performs a scheduled wake-up or
     * timer-based operation. Used to track wake-up frequency.
     */
    virtual void recordWakeUp() = 0;

    /**
     * @brief Get the current wake-up frequency.
     *
     * @return Wake-up frequency in Hz (wake-ups per second)
     */
    virtual double getWakeUpFrequencyHz() const = 0;

    /**
     * @brief Check if wake-up frequency is compliant.
     *
     * In idle mode, wake-up frequency must be <1 Hz to be compliant.
     *
     * @return true if wake-up frequency is within acceptable limits
     */
    virtual bool isWakeUpFrequencyCompliant() const = 0;

    /**
     * @brief Get the total wake-up count.
     *
     * @return Total number of recorded wake-ups
     */
    virtual uint64_t getWakeUpCount() const = 0;

    // =========================================================================
    // Async I/O Status (Requirement 10.2)
    // =========================================================================

    /**
     * @brief Check if async I/O is enabled.
     *
     * Async I/O should always be enabled to avoid busy-wait loops.
     *
     * @return true if platform-native async I/O is enabled
     */
    virtual bool isAsyncIOEnabled() const = 0;

    /**
     * @brief Enable or disable async I/O.
     *
     * @param enabled true to enable async I/O
     */
    virtual void setAsyncIOEnabled(bool enabled) = 0;

    // =========================================================================
    // Network Operation Batching (Requirement 10.3)
    // =========================================================================

    /**
     * @brief Enable or disable network operation batching.
     *
     * When enabled, network operations are queued and flushed together
     * to reduce radio wake-ups.
     *
     * @param enabled true to enable batching
     */
    virtual void enableNetworkBatching(bool enabled) = 0;

    /**
     * @brief Check if network batching is enabled.
     *
     * @return true if network batching is enabled
     */
    virtual bool isNetworkBatchingEnabled() const = 0;

    /**
     * @brief Set the batch flush interval.
     *
     * Maximum time to wait before flushing pending network operations.
     *
     * @param interval Flush interval
     */
    virtual void setBatchFlushInterval(std::chrono::milliseconds interval) = 0;

    /**
     * @brief Get the current batch flush interval.
     *
     * @return Current flush interval
     */
    virtual std::chrono::milliseconds getBatchFlushInterval() const = 0;

    /**
     * @brief Queue a network operation for batching.
     *
     * If batching is disabled, the operation is executed immediately.
     *
     * @param operation The network operation to queue
     * @return Success or error
     */
    virtual core::Result<void, BatteryError> queueNetworkOperation(
        NetworkOperation operation) = 0;

    /**
     * @brief Flush all pending batched operations.
     *
     * Forces immediate execution of all queued network operations.
     *
     * @return Number of operations flushed, or error
     */
    virtual core::Result<uint32_t, BatteryError> flushBatchedOperations() = 0;

    /**
     * @brief Get the number of pending batched operations.
     *
     * @return Number of operations waiting to be flushed
     */
    virtual uint32_t getPendingBatchedOperations() const = 0;

    // =========================================================================
    // Battery Statistics (Requirement 10.4)
    // =========================================================================

    /**
     * @brief Get current battery usage statistics.
     *
     * Returns comprehensive statistics about battery consumption including
     * CPU time, network bytes, and wake-up frequency.
     *
     * @return Battery statistics
     */
    virtual BatteryStatistics getStatistics() const = 0;

    /**
     * @brief Reset battery statistics.
     *
     * Clears all accumulated statistics.
     */
    virtual void resetStatistics() = 0;

    /**
     * @brief Record CPU time usage.
     *
     * @param duration CPU time to record
     */
    virtual void recordCPUTime(std::chrono::milliseconds duration) = 0;

    /**
     * @brief Record network bytes transferred.
     *
     * @param bytes Number of bytes transferred
     */
    virtual void recordNetworkBytes(uint64_t bytes) = 0;

    // =========================================================================
    // App Store Compliance (Requirement 10.5)
    // =========================================================================

    /**
     * @brief Get App Store compliance status.
     *
     * Checks current battery usage against App Store guidelines and
     * returns compliance status with any identified issues.
     *
     * @return Compliance status
     */
    virtual ComplianceStatus getAppStoreComplianceStatus() const = 0;
};

// =============================================================================
// Platform-Specific Implementation (iOS)
// =============================================================================

#if defined(__APPLE__) || defined(OPENRTMP_IOS_TEST)

/**
 * @brief iOS-specific battery optimization implementation.
 *
 * Uses iOS platform APIs for power management:
 * - ProcessInfo for monitoring CPU usage
 * - NSOperationQueue for operation batching
 * - Dispatch sources for low-power timers
 *
 * ## Power Management
 * iOS provides various mechanisms for power-efficient background execution:
 * - Quality of Service (QoS) classes for prioritizing work
 * - Background task expiration handlers
 * - Energy Efficient API for coalescing work
 *
 * ## Thread Safety
 * All methods are thread-safe. Statistics are accumulated atomically.
 */
class iOSBatteryOptimization : public IBatteryOptimization {
public:
    iOSBatteryOptimization();
    ~iOSBatteryOptimization() override;

    // Non-copyable, non-movable
    iOSBatteryOptimization(const iOSBatteryOptimization&) = delete;
    iOSBatteryOptimization& operator=(const iOSBatteryOptimization&) = delete;

    // Power Mode Management
    core::Result<void, BatteryError> setPowerMode(PowerMode mode) override;
    PowerMode getPowerMode() const override;

    // Idle Mode Management
    void enterIdleMode() override;
    void exitIdleMode() override;
    bool isInIdleMode() const override;

    // Stream Activity Tracking
    void onStreamStarted() override;
    void onStreamStopped() override;
    uint32_t getActiveStreamCount() const override;

    // Wake-up Frequency Management
    void recordWakeUp() override;
    double getWakeUpFrequencyHz() const override;
    bool isWakeUpFrequencyCompliant() const override;
    uint64_t getWakeUpCount() const override;

    // Async I/O Status
    bool isAsyncIOEnabled() const override;
    void setAsyncIOEnabled(bool enabled) override;

    // Network Operation Batching
    void enableNetworkBatching(bool enabled) override;
    bool isNetworkBatchingEnabled() const override;
    void setBatchFlushInterval(std::chrono::milliseconds interval) override;
    std::chrono::milliseconds getBatchFlushInterval() const override;
    core::Result<void, BatteryError> queueNetworkOperation(NetworkOperation operation) override;
    core::Result<uint32_t, BatteryError> flushBatchedOperations() override;
    uint32_t getPendingBatchedOperations() const override;

    // Battery Statistics
    BatteryStatistics getStatistics() const override;
    void resetStatistics() override;
    void recordCPUTime(std::chrono::milliseconds duration) override;
    void recordNetworkBytes(uint64_t bytes) override;

    // App Store Compliance
    ComplianceStatus getAppStoreComplianceStatus() const override;

private:
    void startIdleTimer();
    void stopIdleTimer();
    void updateWakeUpFrequency();

    // State
    std::atomic<PowerMode> powerMode_;
    std::atomic<bool> isIdleMode_;
    std::atomic<uint32_t> activeStreamCount_;

    // Wake-up tracking
    std::atomic<uint64_t> wakeUpCount_;
    std::chrono::steady_clock::time_point lastWakeUpTime_;
    std::atomic<double> wakeUpFrequencyHz_;
    mutable std::mutex wakeUpMutex_;

    // Statistics
    std::atomic<uint64_t> cpuTimeMs_;
    std::atomic<uint64_t> networkBytesTransferred_;
    std::atomic<uint64_t> networkOperationsCount_;
    std::atomic<uint64_t> batchedOperationsCount_;
    std::atomic<uint64_t> radioWakeUps_;
    std::atomic<uint64_t> idleTimeMs_;
    std::chrono::steady_clock::time_point idleStartTime_;

    // Network batching
    std::atomic<bool> isAsyncIOEnabled_;
    std::atomic<bool> networkBatchingEnabled_;
    std::atomic<uint32_t> batchFlushIntervalMs_;
    std::vector<NetworkOperation> pendingOperations_;
    mutable std::mutex batchMutex_;

    // Platform-specific handles
    void* idleTimer_;  // Dispatch timer for idle mode
};

#endif // __APPLE__

// =============================================================================
// Platform-Specific Implementation (Android)
// =============================================================================

#if defined(__ANDROID__) || defined(OPENRTMP_ANDROID_TEST)

/**
 * @brief Android-specific battery optimization implementation.
 *
 * Uses Android platform APIs for power management:
 * - PowerManager for device power state
 * - AlarmManager for efficient wake-ups
 * - JobScheduler for deferred work
 *
 * ## Doze Mode Support
 * On Android 6.0+, the system enters Doze mode when idle. This
 * implementation respects Doze mode and reduces activity accordingly.
 *
 * ## Thread Safety
 * All methods are thread-safe. JNI calls are minimized for efficiency.
 */
class AndroidBatteryOptimization : public IBatteryOptimization {
public:
    AndroidBatteryOptimization();
    ~AndroidBatteryOptimization() override;

    // Non-copyable, non-movable
    AndroidBatteryOptimization(const AndroidBatteryOptimization&) = delete;
    AndroidBatteryOptimization& operator=(const AndroidBatteryOptimization&) = delete;

    /**
     * @brief Initialize with JNI environment.
     *
     * @param javaVM Pointer to JavaVM
     * @param applicationContext Global reference to Application Context
     */
    void initializeJNI(void* javaVM, void* applicationContext);

    // Power Mode Management
    core::Result<void, BatteryError> setPowerMode(PowerMode mode) override;
    PowerMode getPowerMode() const override;

    // Idle Mode Management
    void enterIdleMode() override;
    void exitIdleMode() override;
    bool isInIdleMode() const override;

    // Stream Activity Tracking
    void onStreamStarted() override;
    void onStreamStopped() override;
    uint32_t getActiveStreamCount() const override;

    // Wake-up Frequency Management
    void recordWakeUp() override;
    double getWakeUpFrequencyHz() const override;
    bool isWakeUpFrequencyCompliant() const override;
    uint64_t getWakeUpCount() const override;

    // Async I/O Status
    bool isAsyncIOEnabled() const override;
    void setAsyncIOEnabled(bool enabled) override;

    // Network Operation Batching
    void enableNetworkBatching(bool enabled) override;
    bool isNetworkBatchingEnabled() const override;
    void setBatchFlushInterval(std::chrono::milliseconds interval) override;
    std::chrono::milliseconds getBatchFlushInterval() const override;
    core::Result<void, BatteryError> queueNetworkOperation(NetworkOperation operation) override;
    core::Result<uint32_t, BatteryError> flushBatchedOperations() override;
    uint32_t getPendingBatchedOperations() const override;

    // Battery Statistics
    BatteryStatistics getStatistics() const override;
    void resetStatistics() override;
    void recordCPUTime(std::chrono::milliseconds duration) override;
    void recordNetworkBytes(uint64_t bytes) override;

    // App Store Compliance
    ComplianceStatus getAppStoreComplianceStatus() const override;

private:
    void scheduleIdleAlarm();
    void cancelIdleAlarm();
    void updateWakeUpFrequency();

    // State
    std::atomic<PowerMode> powerMode_;
    std::atomic<bool> isIdleMode_;
    std::atomic<uint32_t> activeStreamCount_;

    // Wake-up tracking
    std::atomic<uint64_t> wakeUpCount_;
    std::chrono::steady_clock::time_point lastWakeUpTime_;
    std::atomic<double> wakeUpFrequencyHz_;
    mutable std::mutex wakeUpMutex_;

    // Statistics
    std::atomic<uint64_t> cpuTimeMs_;
    std::atomic<uint64_t> networkBytesTransferred_;
    std::atomic<uint64_t> networkOperationsCount_;
    std::atomic<uint64_t> batchedOperationsCount_;
    std::atomic<uint64_t> radioWakeUps_;
    std::atomic<uint64_t> idleTimeMs_;
    std::chrono::steady_clock::time_point idleStartTime_;

    // Network batching
    std::atomic<bool> isAsyncIOEnabled_;
    std::atomic<bool> networkBatchingEnabled_;
    std::atomic<uint32_t> batchFlushIntervalMs_;
    std::vector<NetworkOperation> pendingOperations_;
    mutable std::mutex batchMutex_;

    // JNI handles
    void* javaVM_;              // JavaVM*
    void* applicationContext_;  // jobject global ref
};

#endif // __ANDROID__

} // namespace mobile
} // namespace pal
} // namespace openrtmp

#endif // OPENRTMP_PAL_MOBILE_BATTERY_OPTIMIZATION_HPP
