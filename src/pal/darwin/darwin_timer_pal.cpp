// OpenRTMP - Cross-platform RTMP Server
// Darwin (macOS/iOS) Timer PAL Implementation

#if defined(__APPLE__)

#include "openrtmp/pal/darwin/darwin_timer_pal.hpp"

#include <dispatch/dispatch.h>
#include <mach/mach_time.h>

namespace openrtmp {
namespace pal {
namespace darwin {

DarwinTimerPAL::DarwinTimerPAL() {
    // Create a serial queue for timer callbacks
    timerQueue_ = static_cast<void*>(
        dispatch_queue_create("com.openrtmp.timers", DISPATCH_QUEUE_SERIAL)
    );
}

DarwinTimerPAL::~DarwinTimerPAL() {
    // Cancel all active timers
    {
        std::lock_guard<std::mutex> lock(timersMutex_);
        for (auto& pair : timers_) {
            if (pair.second.dispatchSource && !pair.second.cancelled) {
                dispatch_source_t source = static_cast<dispatch_source_t>(pair.second.dispatchSource);
                dispatch_source_cancel(source);
                dispatch_release(source);
            }
        }
        timers_.clear();
    }

    // Release the timer queue
    if (timerQueue_) {
        dispatch_release(static_cast<dispatch_queue_t>(timerQueue_));
        timerQueue_ = nullptr;
    }
}

core::Result<TimerHandle, TimerError> DarwinTimerPAL::scheduleOnce(
    std::chrono::milliseconds delay,
    TimerCallback callback)
{
    return createTimer(delay, std::chrono::milliseconds{0}, std::move(callback), false);
}

core::Result<TimerHandle, TimerError> DarwinTimerPAL::scheduleRepeating(
    std::chrono::milliseconds interval,
    TimerCallback callback)
{
    return createTimer(interval, interval, std::move(callback), true);
}

core::Result<TimerHandle, TimerError> DarwinTimerPAL::createTimer(
    std::chrono::milliseconds delay,
    std::chrono::milliseconds interval,
    TimerCallback callback,
    bool repeating)
{
    if (!timerQueue_) {
        return core::Result<TimerHandle, TimerError>::error(
            TimerError{TimerErrorCode::CreationFailed, "Timer queue not initialized"}
        );
    }

    // Create dispatch timer source
    dispatch_source_t timer = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER,
        0,
        0,
        static_cast<dispatch_queue_t>(timerQueue_)
    );

    if (!timer) {
        return core::Result<TimerHandle, TimerError>::error(
            TimerError{TimerErrorCode::CreationFailed, "Failed to create dispatch timer source"}
        );
    }

    // Generate handle
    TimerHandle handle = generateHandle();

    // Store timer info
    {
        std::lock_guard<std::mutex> lock(timersMutex_);
        timers_[handle.value] = TimerInfo{
            static_cast<void*>(timer),
            repeating,
            false
        };
    }

    // Capture handle value for the callback
    uint64_t handleValue = handle.value;

    // Set up the callback
    // We need to capture the callback by value to ensure it survives
    auto callbackCopy = std::make_shared<TimerCallback>(std::move(callback));

    dispatch_source_set_event_handler(timer, ^{
        // Call the user callback
        if (*callbackCopy) {
            (*callbackCopy)();
        }

        // For one-shot timers, mark as cancelled after firing
        if (!repeating) {
            std::lock_guard<std::mutex> lock(timersMutex_);
            auto it = timers_.find(handleValue);
            if (it != timers_.end()) {
                it->second.cancelled = true;
            }
        }
    });

    // Set up cancellation handler
    dispatch_source_set_cancel_handler(timer, ^{
        // Timer has been cancelled, release the source
        // Note: This is called on the timer queue
    });

    // Configure the timer
    dispatch_time_t start = dispatch_time(
        DISPATCH_TIME_NOW,
        static_cast<int64_t>(delay.count()) * NSEC_PER_MSEC
    );

    uint64_t intervalNs = repeating ?
        static_cast<uint64_t>(interval.count()) * NSEC_PER_MSEC :
        DISPATCH_TIME_FOREVER;

    // Leeway of 10% for power efficiency
    uint64_t leeway = repeating ?
        static_cast<uint64_t>(interval.count() / 10) * NSEC_PER_MSEC :
        static_cast<uint64_t>(delay.count() / 10) * NSEC_PER_MSEC;

    if (leeway < NSEC_PER_MSEC) {
        leeway = NSEC_PER_MSEC;  // Minimum 1ms leeway
    }

    dispatch_source_set_timer(timer, start, intervalNs, leeway);

    // Start the timer
    dispatch_resume(timer);

    return core::Result<TimerHandle, TimerError>::success(handle);
}

core::Result<void, TimerError> DarwinTimerPAL::cancelTimer(TimerHandle handle) {
    if (handle == INVALID_TIMER_HANDLE) {
        return core::Result<void, TimerError>::error(
            TimerError{TimerErrorCode::InvalidHandle, "Invalid timer handle"}
        );
    }

    std::lock_guard<std::mutex> lock(timersMutex_);

    auto it = timers_.find(handle.value);
    if (it == timers_.end()) {
        return core::Result<void, TimerError>::error(
            TimerError{TimerErrorCode::InvalidHandle, "Timer not found"}
        );
    }

    if (it->second.cancelled) {
        return core::Result<void, TimerError>::error(
            TimerError{TimerErrorCode::AlreadyCancelled, "Timer already cancelled"}
        );
    }

    // Cancel the dispatch source
    dispatch_source_t source = static_cast<dispatch_source_t>(it->second.dispatchSource);
    dispatch_source_cancel(source);
    dispatch_release(source);

    // Mark as cancelled
    it->second.cancelled = true;
    it->second.dispatchSource = nullptr;

    return core::Result<void, TimerError>::success();
}

std::chrono::steady_clock::time_point DarwinTimerPAL::now() const {
    return std::chrono::steady_clock::now();
}

uint64_t DarwinTimerPAL::getMonotonicMillis() const {
    // Use mach_absolute_time for high-resolution timing
    static mach_timebase_info_data_t timebaseInfo;
    static dispatch_once_t onceToken;

    dispatch_once(&onceToken, ^{
        mach_timebase_info(&timebaseInfo);
    });

    uint64_t machTime = mach_absolute_time();

    // Convert to nanoseconds
    uint64_t nanos = machTime * timebaseInfo.numer / timebaseInfo.denom;

    // Convert to milliseconds
    return nanos / NSEC_PER_MSEC;
}

TimerHandle DarwinTimerPAL::generateHandle() {
    return TimerHandle{nextHandle_.fetch_add(1)};
}

} // namespace darwin
} // namespace pal
} // namespace openrtmp

#endif // __APPLE__
