// OpenRTMP - Cross-platform RTMP Server
// Darwin (macOS/iOS) Timer PAL Implementation
//
// Uses dispatch timers (Grand Central Dispatch) for timer scheduling
// Requirements Covered: 6.5 (Timer abstraction)

#ifndef OPENRTMP_PAL_DARWIN_DARWIN_TIMER_PAL_HPP
#define OPENRTMP_PAL_DARWIN_DARWIN_TIMER_PAL_HPP

#include "openrtmp/pal/timer_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"
#include "openrtmp/core/result.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>

#if defined(__APPLE__)

namespace openrtmp {
namespace pal {
namespace darwin {

/**
 * @brief Darwin (macOS/iOS) implementation of ITimerPAL using dispatch timers.
 *
 * This implementation uses Grand Central Dispatch (GCD) for timer scheduling:
 * - dispatch_source_create with DISPATCH_SOURCE_TYPE_TIMER for timer sources
 * - dispatch_time for computing absolute times
 * - dispatch_source_set_timer for configuring timer parameters
 * - mach_absolute_time for high-resolution timing
 *
 * Thread Safety:
 * - All methods are thread-safe
 * - Timer callbacks are dispatched on a dedicated serial queue
 * - Timer handle management uses mutex for protection
 */
class DarwinTimerPAL : public ITimerPAL {
public:
    /**
     * @brief Construct a Darwin timer PAL.
     *
     * Creates a dedicated dispatch queue for timer callbacks.
     */
    DarwinTimerPAL();

    /**
     * @brief Destructor.
     *
     * Cancels all active timers and releases the dispatch queue.
     */
    ~DarwinTimerPAL() override;

    // Non-copyable, non-movable
    DarwinTimerPAL(const DarwinTimerPAL&) = delete;
    DarwinTimerPAL& operator=(const DarwinTimerPAL&) = delete;
    DarwinTimerPAL(DarwinTimerPAL&&) = delete;
    DarwinTimerPAL& operator=(DarwinTimerPAL&&) = delete;

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
        void* dispatchSource;   // dispatch_source_t
        bool repeating;
        bool cancelled;
    };

    /**
     * @brief Create a dispatch timer.
     */
    core::Result<TimerHandle, TimerError> createTimer(
        std::chrono::milliseconds delay,
        std::chrono::milliseconds interval,  // 0 for one-shot
        TimerCallback callback,
        bool repeating
    );

    /**
     * @brief Generate a unique timer handle.
     */
    TimerHandle generateHandle();

    std::atomic<uint64_t> nextHandle_{1};
    mutable std::mutex timersMutex_;
    std::unordered_map<uint64_t, TimerInfo> timers_;

    // Dispatch queue for timer callbacks (stored as void* to avoid dispatch header)
    void* timerQueue_{nullptr};
};

} // namespace darwin
} // namespace pal
} // namespace openrtmp

#endif // __APPLE__
#endif // OPENRTMP_PAL_DARWIN_DARWIN_TIMER_PAL_HPP
