// OpenRTMP - Cross-platform RTMP Server
// Windows Thread PAL Implementation
//
// Uses Windows threading APIs and thread pools
// Requirements Covered: 6.3 (Threading abstraction)

#ifndef OPENRTMP_PAL_WINDOWS_WINDOWS_THREAD_PAL_HPP
#define OPENRTMP_PAL_WINDOWS_WINDOWS_THREAD_PAL_HPP

#include "openrtmp/pal/thread_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"
#include "openrtmp/core/result.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <queue>
#include <condition_variable>

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
 * @brief Windows implementation of IThreadPAL.
 *
 * This implementation uses:
 * - CreateThread for thread creation
 * - WaitForSingleObject for thread joining
 * - CRITICAL_SECTION for mutexes (fast, recursive)
 * - CONDITION_VARIABLE for condition variables
 * - Windows Thread Pool (TP_WORK/TP_POOL) for thread pools
 * - SetThreadDescription for thread naming (Windows 10+)
 *
 * Thread Safety:
 * - All methods are thread-safe
 * - Handle maps protected by internal mutexes
 */
class WindowsThreadPAL : public IThreadPAL {
public:
    /**
     * @brief Construct a Windows thread PAL.
     */
    WindowsThreadPAL();

    /**
     * @brief Destructor.
     *
     * Note: Does not automatically join/detach threads or destroy pools.
     * Caller is responsible for proper cleanup.
     */
    ~WindowsThreadPAL() override;

    // Non-copyable, non-movable
    WindowsThreadPAL(const WindowsThreadPAL&) = delete;
    WindowsThreadPAL& operator=(const WindowsThreadPAL&) = delete;
    WindowsThreadPAL(WindowsThreadPAL&&) = delete;
    WindowsThreadPAL& operator=(WindowsThreadPAL&&) = delete;

    // =========================================================================
    // Thread Creation and Management
    // =========================================================================

    core::Result<ThreadHandle, ThreadError> createThread(
        ThreadFunction func,
        void* arg,
        const ThreadOptions& options
    ) override;

    core::Result<void, ThreadError> joinThread(ThreadHandle handle) override;

    core::Result<void, ThreadError> detachThread(ThreadHandle handle) override;

    // =========================================================================
    // Thread Pool Operations
    // =========================================================================

    core::Result<ThreadPoolHandle, ThreadError> createThreadPool(
        size_t minThreads,
        size_t maxThreads,
        const ThreadPoolOptions& options
    ) override;

    core::Result<void, ThreadError> submitWork(
        ThreadPoolHandle pool,
        WorkItem work
    ) override;

    core::Result<void, ThreadError> destroyThreadPool(
        ThreadPoolHandle pool
    ) override;

    // =========================================================================
    // Synchronization Primitives
    // =========================================================================

    core::Result<MutexHandle, ThreadError> createMutex() override;

    void destroyMutex(MutexHandle handle) override;

    void lockMutex(MutexHandle handle) override;

    bool tryLockMutex(MutexHandle handle) override;

    void unlockMutex(MutexHandle handle) override;

    core::Result<ConditionVariableHandle, ThreadError> createConditionVariable() override;

    void destroyConditionVariable(ConditionVariableHandle handle) override;

    void waitConditionVariable(ConditionVariableHandle cv, MutexHandle mutex) override;

    bool waitConditionVariableFor(
        ConditionVariableHandle cv,
        MutexHandle mutex,
        std::chrono::milliseconds timeout
    ) override;

    void notifyOne(ConditionVariableHandle cv) override;

    void notifyAll(ConditionVariableHandle cv) override;

    // =========================================================================
    // Current Thread Operations
    // =========================================================================

    ThreadId getCurrentThreadId() override;

    void setThreadName(const std::string& name) override;

    void sleepFor(std::chrono::milliseconds duration) override;

private:
    /**
     * @brief Internal thread data.
     */
    struct ThreadData {
        HANDLE handle;
        ThreadFunction func;
        void* arg;
        std::string name;
        bool joined;
        bool detached;
    };

    /**
     * @brief Internal thread pool data.
     */
    struct ThreadPoolData {
        PTP_POOL pool;
        TP_CALLBACK_ENVIRON callbackEnv;
        PTP_CLEANUP_GROUP cleanupGroup;
        std::string name;
        bool shutdown;
    };

    /**
     * @brief Internal work item for thread pool.
     */
    struct WorkItemData {
        WorkItem work;
        PTP_WORK tpWork;
    };

    /**
     * @brief Generate a unique handle.
     */
    uint64_t generateHandle();

    /**
     * @brief Thread entry point wrapper.
     */
    static DWORD WINAPI threadEntryPoint(LPVOID arg);

    /**
     * @brief Thread pool work callback.
     */
    static VOID CALLBACK workCallback(
        PTP_CALLBACK_INSTANCE instance,
        PVOID context,
        PTP_WORK work
    );

    std::atomic<uint64_t> nextHandle_{1};

    mutable std::mutex threadsMutex_;
    std::unordered_map<uint64_t, ThreadData> threads_;

    mutable std::mutex poolsMutex_;
    std::unordered_map<uint64_t, ThreadPoolData> pools_;

    mutable std::mutex mutexesMutex_;
    std::unordered_map<uint64_t, CRITICAL_SECTION*> mutexes_;

    mutable std::mutex cvsMutex_;
    std::unordered_map<uint64_t, CONDITION_VARIABLE*> conditionVariables_;

    // Work items need to persist until completed
    mutable std::mutex workItemsMutex_;
    std::unordered_map<PTP_WORK, std::unique_ptr<WorkItemData>> workItems_;
};

} // namespace windows
} // namespace pal
} // namespace openrtmp

#endif // _WIN32
#endif // OPENRTMP_PAL_WINDOWS_WINDOWS_THREAD_PAL_HPP
