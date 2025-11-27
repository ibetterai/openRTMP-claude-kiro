// OpenRTMP - Cross-platform RTMP Server
// Windows Timer PAL Implementation
//
// Uses Windows Timer Queues for timer scheduling
// Requirements Covered: 6.5 (Timer abstraction)

#ifndef OPENRTMP_PAL_WINDOWS_WINDOWS_TIMER_PAL_HPP
#define OPENRTMP_PAL_WINDOWS_WINDOWS_TIMER_PAL_HPP

#include "openrtmp/pal/timer_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"
#include "openrtmp/core/result.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <memory>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace openrtmp {
namespace pal {
namespace windows {

/**
 * @brief Windows implementation of ITimerPAL using Timer Queues.
 *
 * This implementation uses:
 * - CreateTimerQueue for creating timer queue
 * - CreateTimerQueueTimer for scheduling timers
 * - DeleteTimerQueueTimer for cancelling timers
 * - QueryPerformanceCounter for high-resolution timing
 *
 * Thread Safety:
 * - All methods are thread-safe
 * - Timer callbacks are dispatched on a system thread pool
 * - Timer handle management uses mutex for protection
 */
class WindowsTimerPAL : public ITimerPAL {
public:
    /**
     * @brief Construct a Windows timer PAL.
     *
     * Creates a timer queue for timer management.
     */
    WindowsTimerPAL();

    /**
     * @brief Destructor.
     *
     * Cancels all active timers and deletes the timer queue.
     */
    ~WindowsTimerPAL() override;

    // Non-copyable, non-movable
    WindowsTimerPAL(const WindowsTimerPAL&) = delete;
    WindowsTimerPAL& operator=(const WindowsTimerPAL&) = delete;
    WindowsTimerPAL(WindowsTimerPAL&&) = delete;
    WindowsTimerPAL& operator=(WindowsTimerPAL&&) = delete;

    // =========================================================================
    // ITimerPAL Implementation
    // =========================================================================

    /**
     * @brief Schedule a one-shot timer.
     */
    core::Result<TimerHandle, TimerError> scheduleOnce(
        std::chrono::milliseconds delay,
        TimerCallback callback
    ) override;

    /**
     * @brief Schedule a repeating timer.
     */
    core::Result<TimerHandle, TimerError> scheduleRepeating(
        std::chrono::milliseconds interval,
        TimerCallback callback
    ) override;

    /**
     * @brief Cancel a timer.
     */
    core::Result<void, TimerError> cancelTimer(TimerHandle handle) override;

    /**
     * @brief Get current monotonic time.
     */
    std::chrono::steady_clock::time_point now() const override;

    /**
     * @brief Get monotonic time in milliseconds.
     */
    uint64_t getMonotonicMillis() const override;

private:
    /**
     * @brief Internal timer information.
     */
    struct TimerInfo {
        HANDLE timerHandle;
        TimerCallback callback;
        bool repeating;
        bool cancelled;
        uint64_t id;  // Our handle ID for reference
    };

    /**
     * @brief Create a timer queue timer.
     */
    core::Result<TimerHandle, TimerError> createTimer(
        std::chrono::milliseconds delay,
        std::chrono::milliseconds period,  // 0 for one-shot
        TimerCallback callback,
        bool repeating
    );

    /**
     * @brief Timer callback wrapper.
     */
    static VOID CALLBACK timerCallback(
        PVOID lpParameter,
        BOOLEAN TimerOrWaitFired
    );

    /**
     * @brief Generate a unique timer handle.
     */
    TimerHandle generateHandle();

    std::atomic<uint64_t> nextHandle_{1};
    mutable std::mutex timersMutex_;
    std::unordered_map<uint64_t, std::shared_ptr<TimerInfo>> timers_;

    // Timer queue handle
    HANDLE timerQueue_ = nullptr;

    // Performance counter frequency for high-resolution timing
    LARGE_INTEGER perfFrequency_;
};

} // namespace windows
} // namespace pal
} // namespace openrtmp

#endif // _WIN32
#endif // OPENRTMP_PAL_WINDOWS_WINDOWS_TIMER_PAL_HPP
