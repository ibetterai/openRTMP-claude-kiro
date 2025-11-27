// OpenRTMP - Cross-platform RTMP Server
// Linux/Android Thread PAL Implementation
//
// Uses pthreads for threading with futex support for efficient synchronization
// Requirements Covered: 6.3 (Threading abstraction)

#ifndef OPENRTMP_PAL_LINUX_LINUX_THREAD_PAL_HPP
#define OPENRTMP_PAL_LINUX_LINUX_THREAD_PAL_HPP

#include "openrtmp/pal/thread_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"
#include "openrtmp/core/result.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <queue>
#include <condition_variable>
#include <memory>

#if defined(__linux__) || defined(__ANDROID__)
#include <pthread.h>

namespace openrtmp {
namespace pal {
namespace linux {

/**
 * @brief Linux/Android implementation of IThreadPAL using pthreads.
 *
 * This implementation uses:
 * - pthreads for thread creation and management
 * - pthread_mutex for mutexes
 * - pthread_cond for condition variables
 * - Custom thread pool implementation
 * - pthread_setname_np for thread naming (Linux-specific)
 * - prctl(PR_SET_NAME) as fallback for thread naming
 *
 * Thread Safety:
 * - All methods are thread-safe
 * - Handle maps protected by internal mutexes
 */
class LinuxThreadPAL : public IThreadPAL {
public:
    /**
     * @brief Construct a Linux thread PAL.
     */
    LinuxThreadPAL();

    /**
     * @brief Destructor.
     *
     * Note: Does not automatically join/detach threads or destroy pools.
     * Caller is responsible for proper cleanup.
     */
    ~LinuxThreadPAL() override;

    // Non-copyable, non-movable
    LinuxThreadPAL(const LinuxThreadPAL&) = delete;
    LinuxThreadPAL& operator=(const LinuxThreadPAL&) = delete;
    LinuxThreadPAL(LinuxThreadPAL&&) = delete;
    LinuxThreadPAL& operator=(LinuxThreadPAL&&) = delete;

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
        pthread_t thread;
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
        std::vector<pthread_t> workers;
        std::queue<WorkItem> workQueue;
        std::mutex queueMutex;
        std::condition_variable workCondition;
        std::atomic<bool> shutdown{false};
        std::string name;
        size_t minThreads;
        size_t maxThreads;
    };

    /**
     * @brief Generate a unique handle.
     */
    uint64_t generateHandle();

    /**
     * @brief Thread entry point wrapper.
     */
    static void* threadEntryPoint(void* arg);

    /**
     * @brief Thread pool worker function.
     */
    static void* poolWorkerFunction(void* arg);

    std::atomic<uint64_t> nextHandle_{1};

    mutable std::mutex threadsMutex_;
    std::unordered_map<uint64_t, std::unique_ptr<ThreadData>> threads_;

    mutable std::mutex poolsMutex_;
    std::unordered_map<uint64_t, std::unique_ptr<ThreadPoolData>> pools_;

    mutable std::mutex mutexesMutex_;
    std::unordered_map<uint64_t, pthread_mutex_t*> mutexes_;

    mutable std::mutex cvsMutex_;
    std::unordered_map<uint64_t, pthread_cond_t*> conditionVariables_;
};

} // namespace linux
} // namespace pal
} // namespace openrtmp

#endif // defined(__linux__) || defined(__ANDROID__)
#endif // OPENRTMP_PAL_LINUX_LINUX_THREAD_PAL_HPP
