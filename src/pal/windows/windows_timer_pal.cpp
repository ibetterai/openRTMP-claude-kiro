// OpenRTMP - Cross-platform RTMP Server
// Windows Timer PAL Implementation

#if defined(_WIN32)

#include "openrtmp/pal/windows/windows_timer_pal.hpp"

namespace openrtmp {
namespace pal {
namespace windows {

// =============================================================================
// WindowsTimerPAL Implementation
// =============================================================================

WindowsTimerPAL::WindowsTimerPAL() {
    // Initialize performance counter frequency for high-resolution timing
    QueryPerformanceFrequency(&perfFrequency_);

    // Create timer queue
    timerQueue_ = CreateTimerQueue();
}

WindowsTimerPAL::~WindowsTimerPAL() {
    // Cancel all timers
    {
        std::lock_guard<std::mutex> lock(timersMutex_);
        for (auto& pair : timers_) {
            if (pair.second->timerHandle != nullptr && !pair.second->cancelled) {
                // Use INVALID_HANDLE_VALUE to wait for completion
                DeleteTimerQueueTimer(timerQueue_, pair.second->timerHandle,
                                      INVALID_HANDLE_VALUE);
            }
        }
        timers_.clear();
    }

    // Delete timer queue
    if (timerQueue_ != nullptr) {
        DeleteTimerQueueEx(timerQueue_, INVALID_HANDLE_VALUE);
        timerQueue_ = nullptr;
    }
}

TimerHandle WindowsTimerPAL::generateHandle() {
    return TimerHandle{nextHandle_.fetch_add(1)};
}

VOID CALLBACK WindowsTimerPAL::timerCallback(PVOID lpParameter, BOOLEAN TimerOrWaitFired) {
    (void)TimerOrWaitFired;

    auto* info = static_cast<TimerInfo*>(lpParameter);
    if (info && !info->cancelled && info->callback) {
        info->callback();
    }
}

core::Result<TimerHandle, TimerError> WindowsTimerPAL::createTimer(
    std::chrono::milliseconds delay,
    std::chrono::milliseconds period,
    TimerCallback callback,
    bool repeating)
{
    if (timerQueue_ == nullptr) {
        return core::Result<TimerHandle, TimerError>::error(
            TimerError{TimerErrorCode::CreationFailed, "Timer queue not initialized", 0}
        );
    }

    TimerHandle handle = generateHandle();

    // Create timer info
    auto info = std::make_shared<TimerInfo>();
    info->callback = std::move(callback);
    info->repeating = repeating;
    info->cancelled = false;
    info->id = handle.value;

    // Store timer info first (will be accessed by callback)
    {
        std::lock_guard<std::mutex> lock(timersMutex_);
        timers_[handle.value] = info;
    }

    // Create timer queue timer
    HANDLE timerHandle;
    BOOL result = CreateTimerQueueTimer(
        &timerHandle,
        timerQueue_,
        timerCallback,
        info.get(),
        static_cast<DWORD>(delay.count()),
        static_cast<DWORD>(period.count()),
        WT_EXECUTEDEFAULT
    );

    if (!result) {
        std::lock_guard<std::mutex> lock(timersMutex_);
        timers_.erase(handle.value);
        return core::Result<TimerHandle, TimerError>::error(
            TimerError{TimerErrorCode::CreationFailed,
                      "Failed to create timer",
                      static_cast<int>(GetLastError())}
        );
    }

    // Update timer info with handle
    info->timerHandle = timerHandle;

    return core::Result<TimerHandle, TimerError>::success(handle);
}

core::Result<TimerHandle, TimerError> WindowsTimerPAL::scheduleOnce(
    std::chrono::milliseconds delay,
    TimerCallback callback)
{
    return createTimer(delay, std::chrono::milliseconds{0}, std::move(callback), false);
}

core::Result<TimerHandle, TimerError> WindowsTimerPAL::scheduleRepeating(
    std::chrono::milliseconds interval,
    TimerCallback callback)
{
    return createTimer(interval, interval, std::move(callback), true);
}

core::Result<void, TimerError> WindowsTimerPAL::cancelTimer(TimerHandle handle) {
    if (handle == INVALID_TIMER_HANDLE) {
        return core::Result<void, TimerError>::error(
            TimerError{TimerErrorCode::InvalidHandle, "Invalid timer handle", 0}
        );
    }

    std::shared_ptr<TimerInfo> info;
    {
        std::lock_guard<std::mutex> lock(timersMutex_);
        auto it = timers_.find(handle.value);
        if (it == timers_.end()) {
            return core::Result<void, TimerError>::error(
                TimerError{TimerErrorCode::InvalidHandle, "Timer not found", 0}
            );
        }

        info = it->second;

        if (info->cancelled) {
            return core::Result<void, TimerError>::error(
                TimerError{TimerErrorCode::AlreadyCancelled, "Timer already cancelled", 0}
            );
        }

        info->cancelled = true;
    }

    // Delete the timer
    if (info->timerHandle != nullptr) {
        // Don't wait for completion (pass NULL instead of INVALID_HANDLE_VALUE)
        // to avoid potential deadlock if called from timer callback
        DeleteTimerQueueTimer(timerQueue_, info->timerHandle, NULL);
    }

    // Remove from map
    {
        std::lock_guard<std::mutex> lock(timersMutex_);
        timers_.erase(handle.value);
    }

    return core::Result<void, TimerError>::success();
}

std::chrono::steady_clock::time_point WindowsTimerPAL::now() const {
    return std::chrono::steady_clock::now();
}

uint64_t WindowsTimerPAL::getMonotonicMillis() const {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);

    // Convert to milliseconds
    return static_cast<uint64_t>(
        (counter.QuadPart * 1000) / perfFrequency_.QuadPart
    );
}

} // namespace windows
} // namespace pal
} // namespace openrtmp

#endif // _WIN32
