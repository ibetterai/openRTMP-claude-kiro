// OpenRTMP - Cross-platform RTMP Server
// Platform Abstraction Layer - Timer Interface
//
// This interface abstracts platform-specific timer mechanisms for
// scheduling callbacks and high-resolution time measurement.
// Implementations use:
// - dispatch timers on macOS/iOS
// - timerfd on Linux
// - timer queue on Windows
// - Handler/Looper on Android
//
// Requirements Covered: 6.5 (timer abstraction)

#ifndef OPENRTMP_PAL_TIMER_PAL_HPP
#define OPENRTMP_PAL_TIMER_PAL_HPP

#include "openrtmp/pal/pal_types.hpp"
#include "openrtmp/core/result.hpp"

#include <chrono>

namespace openrtmp {
namespace pal {

/**
 * @brief Abstract interface for platform-specific timer operations.
 *
 * This interface provides a unified API for scheduling callbacks and
 * accessing high-resolution time across all supported platforms.
 *
 * ## Timer Types
 * - **One-shot timers**: Fire once after a delay
 * - **Repeating timers**: Fire at regular intervals
 *
 * ## Thread Safety
 * - Timer scheduling methods are thread-safe
 * - Callbacks are invoked on the event loop thread or a timer thread
 *   (platform-dependent)
 * - Callbacks must be short-lived to avoid blocking other timers
 *
 * ## Timer Accuracy
 * - Timers are not guaranteed to fire at exactly the specified time
 * - Actual delay may be slightly longer due to system load
 * - For high-precision timing, use now() and getMonotonicMillis()
 *
 * @invariant Timer handles are valid until cancelled or (for one-shot) fired
 * @invariant now() returns monotonically increasing values
 */
class ITimerPAL {
public:
    virtual ~ITimerPAL() = default;

    // =========================================================================
    // Timer Scheduling
    // =========================================================================

    /**
     * @brief Schedule a one-shot timer.
     *
     * Schedules a callback to be invoked once after the specified delay.
     * After the callback is invoked, the timer is automatically cancelled.
     *
     * @param delay Time to wait before invoking the callback
     * @param callback Function to invoke when the timer fires
     *
     * @pre delay >= 0 (zero delay fires as soon as possible)
     * @pre callback is valid and callable
     *
     * @return TimerHandle for cancellation, or TimerError on failure
     *
     * @note Timer may fire slightly after the specified delay
     * @note Callback is invoked on timer/event loop thread
     *
     * @code
     * // Handshake timeout
     * auto result = timerPal->scheduleOnce(std::chrono::seconds{10}, [connection]() {
     *     if (!connection->isHandshakeComplete()) {
     *         log("Handshake timeout for connection " + connection->getId());
     *         connection->close();
     *     }
     * });
     *
     * if (result.isSuccess()) {
     *     connection->setHandshakeTimer(result.value());
     * }
     * @endcode
     */
    virtual core::Result<TimerHandle, TimerError> scheduleOnce(
        std::chrono::milliseconds delay,
        TimerCallback callback
    ) = 0;

    /**
     * @brief Schedule a repeating timer.
     *
     * Schedules a callback to be invoked repeatedly at the specified
     * interval. The first invocation occurs after one interval has elapsed.
     *
     * @param interval Time between callback invocations
     * @param callback Function to invoke each time the timer fires
     *
     * @pre interval > 0
     * @pre callback is valid and callable
     *
     * @return TimerHandle for cancellation, or TimerError on failure
     *
     * @note Timer interval is measured from start of each callback
     * @note Long-running callbacks may cause timer drift
     * @note Timer must be explicitly cancelled to stop
     *
     * @code
     * // Periodic statistics logging
     * auto result = timerPal->scheduleRepeating(std::chrono::seconds{60}, []() {
     *     logStatistics();
     * });
     *
     * if (result.isError()) {
     *     log("Failed to create stats timer: " + result.error().message);
     * }
     * @endcode
     */
    virtual core::Result<TimerHandle, TimerError> scheduleRepeating(
        std::chrono::milliseconds interval,
        TimerCallback callback
    ) = 0;

    /**
     * @brief Cancel a scheduled timer.
     *
     * Cancels a timer, preventing any future callbacks. If the timer
     * callback is currently executing, this call waits for it to complete.
     *
     * @param handle Timer to cancel
     *
     * @pre handle is a valid timer handle
     * @post Timer will not fire again
     * @post handle is no longer valid
     *
     * @return Success or TimerError on failure
     *
     * Error conditions:
     * - InvalidHandle: Timer handle is not valid
     * - AlreadyCancelled: Timer was already cancelled
     *
     * @note Safe to call on already-cancelled timers (returns AlreadyCancelled)
     * @note May block briefly if callback is currently executing
     *
     * @code
     * // Cancel handshake timeout on successful handshake
     * if (connection->getHandshakeTimer() != INVALID_TIMER_HANDLE) {
     *     timerPal->cancelTimer(connection->getHandshakeTimer());
     *     connection->setHandshakeTimer(INVALID_TIMER_HANDLE);
     * }
     * @endcode
     */
    virtual core::Result<void, TimerError> cancelTimer(TimerHandle handle) = 0;

    // =========================================================================
    // Time Measurement
    // =========================================================================

    /**
     * @brief Get the current monotonic time.
     *
     * Returns the current time from a monotonic clock that is not affected
     * by system time changes. This is suitable for measuring elapsed time.
     *
     * @return Current time point from a steady clock
     *
     * @note Guaranteed to be monotonically increasing (never goes backwards)
     * @note Resolution is typically microseconds or better
     *
     * @code
     * auto start = timerPal->now();
     * // ... perform operation ...
     * auto end = timerPal->now();
     *
     * auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
     * log("Operation took " + std::to_string(elapsed.count()) + "ms");
     * @endcode
     */
    virtual std::chrono::steady_clock::time_point now() const = 0;

    /**
     * @brief Get monotonic time in milliseconds.
     *
     * Returns the current monotonic time as a simple millisecond counter.
     * This is useful for RTMP timestamp calculations.
     *
     * @return Milliseconds since an unspecified epoch (typically system boot)
     *
     * @note Value wraps around after ~49 days on 32-bit, ~584 million years on 64-bit
     * @note Guaranteed to be monotonically increasing
     *
     * @code
     * uint64_t timestamp = timerPal->getMonotonicMillis();
     * rtmpChunk.setTimestamp(static_cast<uint32_t>(timestamp));
     * @endcode
     */
    virtual uint64_t getMonotonicMillis() const = 0;
};

} // namespace pal
} // namespace openrtmp

#endif // OPENRTMP_PAL_TIMER_PAL_HPP
