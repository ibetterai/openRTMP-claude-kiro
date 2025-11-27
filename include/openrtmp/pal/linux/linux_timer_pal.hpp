// OpenRTMP - Cross-platform RTMP Server
// Linux/Android Timer PAL Implementation
//
// Uses timerfd for timer scheduling
// Requirements Covered: 6.5 (Timer abstraction)

#ifndef OPENRTMP_PAL_LINUX_LINUX_TIMER_PAL_HPP
#define OPENRTMP_PAL_LINUX_LINUX_TIMER_PAL_HPP

#include "openrtmp/pal/timer_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"
#include "openrtmp/core/result.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <condition_variable>

#if defined(__linux__) || defined(__ANDROID__)

namespace openrtmp {
namespace pal {
namespace linux {

/**
 * @brief Linux/Android implementation of ITimerPAL using timerfd.
 *
 * This implementation uses:
 * - timerfd_create for creating timer file descriptors
 * - timerfd_settime for configuring timer parameters
 * - epoll for monitoring timer events
 * - clock_gettime(CLOCK_MONOTONIC) for high-resolution timing
 *
 * Thread Safety:
 * - All methods are thread-safe
 * - Timer callbacks are dispatched on a dedicated timer thread
 * - Timer handle management uses mutex for protection
 */
class LinuxTimerPAL : public ITimerPAL {
public:
    /**
     * @brief Construct a Linux timer PAL.
     *
     * Creates a dedicated thread for timer callbacks.
     */
    LinuxTimerPAL();

    /**
     * @brief Destructor.
     *
     * Cancels all active timers and releases resources.
     */
    ~LinuxTimerPAL() override;

    // Non-copyable, non-movable
    LinuxTimerPAL(const LinuxTimerPAL&) = delete;
    LinuxTimerPAL& operator=(const LinuxTimerPAL&) = delete;
    LinuxTimerPAL(LinuxTimerPAL&&) = delete;
    LinuxTimerPAL& operator=(LinuxTimerPAL&&) = delete;

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
        int timerFd;
        TimerCallback callback;
        bool repeating;
        std::chrono::milliseconds interval;
        std::atomic<bool> cancelled{false};
    };

    /**
     * @brief Create a timerfd-based timer.
     */
    core::Result<TimerHandle, TimerError> createTimer(
        std::chrono::milliseconds delay,
        std::chrono::milliseconds interval,
        TimerCallback callback,
        bool repeating
    );

    /**
     * @brief Generate a unique timer handle.
     */
    TimerHandle generateHandle();

    /**
     * @brief Timer thread function.
     */
    void timerThreadFunc();

    /**
     * @brief Wake up the timer thread to handle new timers or shutdown.
     */
    void wakeTimerThread();

    std::atomic<uint64_t> nextHandle_{1};
    mutable std::mutex timersMutex_;
    std::unordered_map<uint64_t, std::unique_ptr<TimerInfo>> timers_;
    std::unordered_map<int, uint64_t> fdToHandle_;  // Map timerfd to handle

    // epoll fd for monitoring timer events
    int epollFd_{-1};

    // Eventfd for waking up the timer thread
    int wakeEventFd_{-1};

    // Timer thread
    std::thread timerThread_;
    std::atomic<bool> shutdown_{false};
};

} // namespace linux
} // namespace pal
} // namespace openrtmp

#endif // defined(__linux__) || defined(__ANDROID__)
#endif // OPENRTMP_PAL_LINUX_LINUX_TIMER_PAL_HPP
