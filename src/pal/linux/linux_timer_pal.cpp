// OpenRTMP - Cross-platform RTMP Server
// Linux/Android Timer PAL Implementation

#include "openrtmp/pal/linux/linux_timer_pal.hpp"

#if defined(__linux__) || defined(__ANDROID__)

#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstring>
#include <errno.h>

namespace openrtmp {
namespace pal {
namespace linux {

// =============================================================================
// Constructor / Destructor
// =============================================================================

LinuxTimerPAL::LinuxTimerPAL() {
    // Create epoll instance for monitoring timer events
    epollFd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epollFd_ < 0) {
        // Log error but continue - timers will fail to schedule
        return;
    }

    // Create eventfd for waking up the timer thread
    wakeEventFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeEventFd_ < 0) {
        close(epollFd_);
        epollFd_ = -1;
        return;
    }

    // Add wakeEventFd to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = wakeEventFd_;
    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeEventFd_, &ev) < 0) {
        close(wakeEventFd_);
        close(epollFd_);
        wakeEventFd_ = -1;
        epollFd_ = -1;
        return;
    }

    // Start timer thread
    timerThread_ = std::thread(&LinuxTimerPAL::timerThreadFunc, this);
}

LinuxTimerPAL::~LinuxTimerPAL() {
    // Signal shutdown
    shutdown_ = true;
    wakeTimerThread();

    // Wait for timer thread to finish
    if (timerThread_.joinable()) {
        timerThread_.join();
    }

    // Close all timer fds and clean up
    {
        std::lock_guard<std::mutex> lock(timersMutex_);
        for (auto& pair : timers_) {
            if (pair.second->timerFd >= 0) {
                close(pair.second->timerFd);
            }
        }
        timers_.clear();
        fdToHandle_.clear();
    }

    // Close wake event fd
    if (wakeEventFd_ >= 0) {
        close(wakeEventFd_);
        wakeEventFd_ = -1;
    }

    // Close epoll fd
    if (epollFd_ >= 0) {
        close(epollFd_);
        epollFd_ = -1;
    }
}

// =============================================================================
// Timer Thread
// =============================================================================

void LinuxTimerPAL::timerThreadFunc() {
    const int maxEvents = 32;
    struct epoll_event events[maxEvents];

    while (!shutdown_) {
        int nfds = epoll_wait(epollFd_, events, maxEvents, -1);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            // Check if it's the wake event
            if (fd == wakeEventFd_) {
                uint64_t val;
                // Drain the eventfd
                while (read(wakeEventFd_, &val, sizeof(val)) > 0) {
                    // Just drain
                }
                continue;
            }

            // Read the timer expiration count
            uint64_t expirations = 0;
            ssize_t bytesRead = read(fd, &expirations, sizeof(expirations));
            if (bytesRead != sizeof(expirations)) {
                continue;
            }

            // Find the timer and invoke callback
            TimerCallback callback;
            bool repeating = false;
            uint64_t handleValue = 0;

            {
                std::lock_guard<std::mutex> lock(timersMutex_);
                auto handleIt = fdToHandle_.find(fd);
                if (handleIt == fdToHandle_.end()) {
                    continue;
                }
                handleValue = handleIt->second;

                auto timerIt = timers_.find(handleValue);
                if (timerIt == timers_.end()) {
                    continue;
                }

                if (timerIt->second->cancelled.load()) {
                    continue;
                }

                callback = timerIt->second->callback;
                repeating = timerIt->second->repeating;
            }

            // Invoke callback outside the lock
            if (callback) {
                callback();
            }

            // For one-shot timers, clean up after firing
            if (!repeating) {
                std::lock_guard<std::mutex> lock(timersMutex_);
                auto timerIt = timers_.find(handleValue);
                if (timerIt != timers_.end()) {
                    epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                    fdToHandle_.erase(fd);
                    timers_.erase(timerIt);
                }
            }
        }
    }
}

void LinuxTimerPAL::wakeTimerThread() {
    if (wakeEventFd_ >= 0) {
        uint64_t val = 1;
        ssize_t result = write(wakeEventFd_, &val, sizeof(val));
        (void)result;  // Ignore result
    }
}

// =============================================================================
// Timer Handle Generation
// =============================================================================

TimerHandle LinuxTimerPAL::generateHandle() {
    return TimerHandle{nextHandle_.fetch_add(1, std::memory_order_relaxed)};
}

// =============================================================================
// Timer Scheduling
// =============================================================================

core::Result<TimerHandle, TimerError> LinuxTimerPAL::createTimer(
    std::chrono::milliseconds delay,
    std::chrono::milliseconds interval,
    TimerCallback callback,
    bool repeating
) {
    if (epollFd_ < 0) {
        return core::Result<TimerHandle, TimerError>::error(
            TimerError{TimerErrorCode::CreationFailed, "Timer subsystem not initialized"}
        );
    }

    // Create timerfd
    int timerFd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerFd < 0) {
        return core::Result<TimerHandle, TimerError>::error(
            TimerError{TimerErrorCode::CreationFailed, "timerfd_create failed: " + std::string(strerror(errno))}
        );
    }

    // Configure timer
    struct itimerspec its;
    memset(&its, 0, sizeof(its));

    // Initial delay
    its.it_value.tv_sec = delay.count() / 1000;
    its.it_value.tv_nsec = (delay.count() % 1000) * 1000000;

    // If delay is 0, set a minimal time to ensure it fires
    if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0) {
        its.it_value.tv_nsec = 1;
    }

    // Interval for repeating timers
    if (repeating && interval.count() > 0) {
        its.it_interval.tv_sec = interval.count() / 1000;
        its.it_interval.tv_nsec = (interval.count() % 1000) * 1000000;
    }

    if (timerfd_settime(timerFd, 0, &its, nullptr) < 0) {
        close(timerFd);
        return core::Result<TimerHandle, TimerError>::error(
            TimerError{TimerErrorCode::CreationFailed, "timerfd_settime failed: " + std::string(strerror(errno))}
        );
    }

    TimerHandle handle = generateHandle();

    // Create timer info
    auto timerInfo = std::make_unique<TimerInfo>();
    timerInfo->timerFd = timerFd;
    timerInfo->callback = std::move(callback);
    timerInfo->repeating = repeating;
    timerInfo->interval = interval;

    // Add to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = timerFd;
    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, timerFd, &ev) < 0) {
        close(timerFd);
        return core::Result<TimerHandle, TimerError>::error(
            TimerError{TimerErrorCode::CreationFailed, "epoll_ctl failed: " + std::string(strerror(errno))}
        );
    }

    {
        std::lock_guard<std::mutex> lock(timersMutex_);
        fdToHandle_[timerFd] = handle.value;
        timers_[handle.value] = std::move(timerInfo);
    }

    return core::Result<TimerHandle, TimerError>::success(handle);
}

core::Result<TimerHandle, TimerError> LinuxTimerPAL::scheduleOnce(
    std::chrono::milliseconds delay,
    TimerCallback callback
) {
    return createTimer(delay, std::chrono::milliseconds{0}, std::move(callback), false);
}

core::Result<TimerHandle, TimerError> LinuxTimerPAL::scheduleRepeating(
    std::chrono::milliseconds interval,
    TimerCallback callback
) {
    return createTimer(interval, interval, std::move(callback), true);
}

core::Result<void, TimerError> LinuxTimerPAL::cancelTimer(TimerHandle handle) {
    if (handle == INVALID_TIMER_HANDLE) {
        return core::Result<void, TimerError>::error(
            TimerError{TimerErrorCode::InvalidHandle, "Invalid timer handle"}
        );
    }

    std::lock_guard<std::mutex> lock(timersMutex_);

    auto it = timers_.find(handle.value);
    if (it == timers_.end()) {
        return core::Result<void, TimerError>::error(
            TimerError{TimerErrorCode::AlreadyCancelled, "Timer not found or already cancelled"}
        );
    }

    // Mark as cancelled
    it->second->cancelled = true;

    // Remove from epoll and close fd
    int fd = it->second->timerFd;
    epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);

    // Remove from maps
    fdToHandle_.erase(fd);
    timers_.erase(it);

    return core::Result<void, TimerError>::success();
}

// =============================================================================
// Time Measurement
// =============================================================================

std::chrono::steady_clock::time_point LinuxTimerPAL::now() const {
    return std::chrono::steady_clock::now();
}

uint64_t LinuxTimerPAL::getMonotonicMillis() const {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec) / 1000000;
}

} // namespace linux
} // namespace pal
} // namespace openrtmp

#endif // defined(__linux__) || defined(__ANDROID__)
