// OpenRTMP - Cross-platform RTMP Server
// Darwin (macOS/iOS) Thread PAL Implementation
//
// Uses pthreads for threading and Grand Central Dispatch for thread pools
// Requirements Covered: 6.3 (Threading abstraction)

#ifndef OPENRTMP_PAL_DARWIN_DARWIN_THREAD_PAL_HPP
#define OPENRTMP_PAL_DARWIN_DARWIN_THREAD_PAL_HPP

#include "openrtmp/pal/thread_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"
#include "openrtmp/core/result.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <queue>
#include <condition_variable>

#if defined(__APPLE__)
#include <pthread.h>

namespace openrtmp {
namespace pal {
namespace darwin {

/**
 * @brief Darwin (macOS/iOS) implementation of IThreadPAL.
 *
 * This implementation uses:
 * - pthreads for thread creation and management
 * - pthread_mutex for mutexes
 * - pthread_cond for condition variables
 * - Grand Central Dispatch (GCD) for thread pools
 * - pthread_setname_np for thread naming
 *
 * Thread Safety:
 * - All methods are thread-safe
 * - Handle maps protected by internal mutexes
 */
class DarwinThreadPAL : public IThreadPAL {
public:
    /**
     * @brief Construct a Darwin thread PAL.
     */
    DarwinThreadPAL();

    /**
     * @brief Destructor.
     *
     * Note: Does not automatically join/detach threads or destroy pools.
     * Caller is responsible for proper cleanup.
     */
    ~DarwinThreadPAL() override;

    // Non-copyable, non-movable
    DarwinThreadPAL(const DarwinThreadPAL&) = delete;
    DarwinThreadPAL& operator=(const DarwinThreadPAL&) = delete;
    DarwinThreadPAL(DarwinThreadPAL&&) = delete;
    DarwinThreadPAL& operator=(DarwinThreadPAL&&) = delete;

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
        void* dispatchQueue;  // dispatch_queue_t
        std::string name;
        bool shutdown;
    };

    /**
     * @brief Generate a unique handle.
     */
    uint64_t generateHandle();

    /**
     * @brief Thread entry point wrapper.
     */
    static void* threadEntryPoint(void* arg);

    std::atomic<uint64_t> nextHandle_{1};

    mutable std::mutex threadsMutex_;
    std::unordered_map<uint64_t, ThreadData> threads_;

    mutable std::mutex poolsMutex_;
    std::unordered_map<uint64_t, ThreadPoolData> pools_;

    mutable std::mutex mutexesMutex_;
    std::unordered_map<uint64_t, pthread_mutex_t*> mutexes_;

    mutable std::mutex cvsMutex_;
    std::unordered_map<uint64_t, pthread_cond_t*> conditionVariables_;
};

} // namespace darwin
} // namespace pal
} // namespace openrtmp

#endif // __APPLE__
#endif // OPENRTMP_PAL_DARWIN_DARWIN_THREAD_PAL_HPP
