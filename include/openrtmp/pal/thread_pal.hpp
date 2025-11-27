// OpenRTMP - Cross-platform RTMP Server
// Platform Abstraction Layer - Threading Interface
//
// This interface abstracts platform-specific threading primitives including
// thread creation, thread pools, and synchronization primitives.
// Implementations use:
// - pthreads on Unix-like systems (macOS, iOS, Linux, Android)
// - Windows threads on Windows
//
// Requirements Covered: 6.3 (threading abstraction)

#ifndef OPENRTMP_PAL_THREAD_PAL_HPP
#define OPENRTMP_PAL_THREAD_PAL_HPP

#include "openrtmp/pal/pal_types.hpp"
#include "openrtmp/core/result.hpp"

#include <chrono>
#include <string>

namespace openrtmp {
namespace pal {

/**
 * @brief Abstract interface for platform-specific threading operations.
 *
 * This interface provides a unified API for thread management, thread pools,
 * and synchronization primitives across all supported platforms.
 *
 * ## Thread Safety
 * - All methods are thread-safe unless otherwise noted
 * - Synchronization primitive operations (lock/unlock) follow standard semantics
 *
 * ## Thread Lifecycle
 * 1. Create thread with createThread()
 * 2. Thread executes the provided function
 * 3. Either joinThread() to wait for completion, or detachThread() to release
 *
 * ## Thread Pool Lifecycle
 * 1. Create pool with createThreadPool()
 * 2. Submit work items with submitWork()
 * 3. Destroy pool with destroyThreadPool() (waits for pending work)
 *
 * @invariant Thread handles are valid until joined, detached, or the thread exits
 * @invariant Mutex handles are valid until destroyed
 * @invariant Condition variable handles are valid until destroyed
 */
class IThreadPAL {
public:
    virtual ~IThreadPAL() = default;

    // =========================================================================
    // Thread Creation and Management
    // =========================================================================

    /**
     * @brief Create and start a new thread.
     *
     * Creates a new thread that executes the provided function with the
     * given argument. The thread starts executing immediately.
     *
     * @param func Function to execute in the new thread
     * @param arg Argument passed to the function
     * @param options Thread configuration options
     *
     * @pre func is a valid, non-null function
     *
     * @return ThreadHandle on success, or ThreadError on failure
     *
     * Error conditions:
     * - CreationFailed: System could not create the thread
     * - ResourceExhausted: System thread limit reached
     * - OutOfMemory: Insufficient memory for thread stack
     *
     * @code
     * struct WorkerContext {
     *     int workerId;
     *     std::atomic<bool>* shouldStop;
     * };
     *
     * void workerFunction(void* arg) {
     *     auto* ctx = static_cast<WorkerContext*>(arg);
     *     while (!ctx->shouldStop->load()) {
     *         doWork(ctx->workerId);
     *     }
     * }
     *
     * WorkerContext ctx{1, &shouldStop};
     * ThreadOptions opts;
     * opts.name = "Worker-1";
     *
     * auto result = threadPal->createThread(workerFunction, &ctx, opts);
     * if (result.isError()) {
     *     log("Failed to create worker thread: " + result.error().message);
     * }
     * @endcode
     */
    virtual core::Result<ThreadHandle, ThreadError> createThread(
        ThreadFunction func,
        void* arg,
        const ThreadOptions& options
    ) = 0;

    /**
     * @brief Wait for a thread to finish execution.
     *
     * Blocks the calling thread until the specified thread terminates.
     * After this call, the thread handle is no longer valid.
     *
     * @param handle Thread to wait for
     *
     * @pre handle is a valid, non-detached thread handle
     * @post Thread resources are released
     * @post handle is no longer valid
     *
     * @return Success or ThreadError on failure
     *
     * Error conditions:
     * - InvalidHandle: Thread handle is not valid
     * - DeadlockDetected: Thread is trying to join itself
     */
    virtual core::Result<void, ThreadError> joinThread(ThreadHandle handle) = 0;

    /**
     * @brief Detach a thread.
     *
     * Marks the thread as detached. When a detached thread terminates,
     * its resources are automatically released. A detached thread cannot
     * be joined.
     *
     * @param handle Thread to detach
     *
     * @pre handle is a valid, non-detached thread handle
     * @post Thread will clean up automatically when it exits
     * @post handle should not be used again
     *
     * @return Success or ThreadError on failure
     */
    virtual core::Result<void, ThreadError> detachThread(ThreadHandle handle) = 0;

    // =========================================================================
    // Thread Pool Operations
    // =========================================================================

    /**
     * @brief Create a thread pool.
     *
     * Creates a pool of worker threads that can execute work items
     * submitted via submitWork(). The pool manages thread lifecycle
     * and work distribution.
     *
     * @param minThreads Minimum number of threads to keep alive
     * @param maxThreads Maximum number of threads in the pool
     * @param options Thread pool configuration
     *
     * @pre minThreads > 0
     * @pre maxThreads >= minThreads
     *
     * @return ThreadPoolHandle on success, or ThreadError on failure
     *
     * @code
     * ThreadPoolOptions opts;
     * opts.name = "WorkerPool";
     * opts.keepAliveTime = std::chrono::seconds{30};
     *
     * auto result = threadPal->createThreadPool(2, 8, opts);
     * if (result.isError()) {
     *     log("Failed to create thread pool: " + result.error().message);
     *     return;
     * }
     *
     * ThreadPoolHandle pool = result.value();
     * @endcode
     */
    virtual core::Result<ThreadPoolHandle, ThreadError> createThreadPool(
        size_t minThreads,
        size_t maxThreads,
        const ThreadPoolOptions& options
    ) = 0;

    /**
     * @brief Submit work to a thread pool.
     *
     * Adds a work item to the pool's queue. The work will be executed
     * by one of the pool's worker threads when one becomes available.
     *
     * @param pool Thread pool to submit work to
     * @param work Work item to execute
     *
     * @pre pool is a valid thread pool handle
     * @pre work is a valid, non-null function
     *
     * @return Success or ThreadError on failure
     *
     * Error conditions:
     * - InvalidHandle: Pool handle is not valid
     * - PoolShutdown: Pool is being shut down
     * - WorkQueueFull: Work queue has reached capacity
     *
     * @code
     * auto result = threadPal->submitWork(pool, [clientId]() {
     *     processClient(clientId);
     * });
     *
     * if (result.isError()) {
     *     log("Failed to submit work: " + result.error().message);
     * }
     * @endcode
     */
    virtual core::Result<void, ThreadError> submitWork(
        ThreadPoolHandle pool,
        WorkItem work
    ) = 0;

    /**
     * @brief Destroy a thread pool.
     *
     * Shuts down the thread pool. Waits for all currently executing
     * work items to complete, but discards pending work items.
     *
     * @param pool Thread pool to destroy
     *
     * @pre pool is a valid thread pool handle
     * @post All threads are terminated
     * @post pool handle is no longer valid
     *
     * @return Success or ThreadError on failure
     */
    virtual core::Result<void, ThreadError> destroyThreadPool(
        ThreadPoolHandle pool
    ) = 0;

    // =========================================================================
    // Synchronization Primitives
    // =========================================================================

    /**
     * @brief Create a mutex.
     *
     * Creates a mutual exclusion lock for protecting shared resources.
     *
     * @return MutexHandle on success, or ThreadError on failure
     *
     * @code
     * auto result = threadPal->createMutex();
     * if (result.isError()) {
     *     log("Failed to create mutex: " + result.error().message);
     *     return;
     * }
     *
     * MutexHandle mutex = result.value();
     * @endcode
     */
    virtual core::Result<MutexHandle, ThreadError> createMutex() = 0;

    /**
     * @brief Destroy a mutex.
     *
     * Releases resources associated with the mutex.
     *
     * @param handle Mutex to destroy
     *
     * @pre handle is a valid mutex handle
     * @pre Mutex is not currently locked
     * @post handle is no longer valid
     */
    virtual void destroyMutex(MutexHandle handle) = 0;

    /**
     * @brief Lock a mutex.
     *
     * Acquires the mutex, blocking if necessary until it becomes available.
     *
     * @param handle Mutex to lock
     *
     * @pre handle is a valid mutex handle
     * @post Calling thread owns the mutex
     *
     * @warning Locking a mutex that the calling thread already owns may
     *          result in undefined behavior depending on the platform.
     */
    virtual void lockMutex(MutexHandle handle) = 0;

    /**
     * @brief Try to lock a mutex without blocking.
     *
     * @param handle Mutex to lock
     *
     * @pre handle is a valid mutex handle
     *
     * @return true if mutex was acquired, false if already locked
     */
    virtual bool tryLockMutex(MutexHandle handle) = 0;

    /**
     * @brief Unlock a mutex.
     *
     * Releases the mutex, allowing other threads to acquire it.
     *
     * @param handle Mutex to unlock
     *
     * @pre handle is a valid mutex handle
     * @pre Calling thread owns the mutex
     */
    virtual void unlockMutex(MutexHandle handle) = 0;

    /**
     * @brief Create a condition variable.
     *
     * Creates a condition variable for thread synchronization.
     *
     * @return ConditionVariableHandle on success, or ThreadError on failure
     */
    virtual core::Result<ConditionVariableHandle, ThreadError> createConditionVariable() = 0;

    /**
     * @brief Destroy a condition variable.
     *
     * @param handle Condition variable to destroy
     *
     * @pre handle is a valid condition variable handle
     * @pre No threads are waiting on this condition variable
     * @post handle is no longer valid
     */
    virtual void destroyConditionVariable(ConditionVariableHandle handle) = 0;

    /**
     * @brief Wait on a condition variable.
     *
     * Atomically releases the mutex and waits for the condition variable
     * to be signaled. Upon return, the mutex is re-acquired.
     *
     * @param cv Condition variable to wait on
     * @param mutex Mutex that must be held when calling this function
     *
     * @pre mutex is locked by the calling thread
     * @post mutex is locked by the calling thread
     *
     * @note May return spuriously; always use a predicate loop
     *
     * @code
     * threadPal->lockMutex(mutex);
     * while (!condition) {
     *     threadPal->waitConditionVariable(cv, mutex);
     * }
     * // Process...
     * threadPal->unlockMutex(mutex);
     * @endcode
     */
    virtual void waitConditionVariable(ConditionVariableHandle cv, MutexHandle mutex) = 0;

    /**
     * @brief Wait on a condition variable with timeout.
     *
     * Like waitConditionVariable, but returns after the specified timeout
     * if the condition variable is not signaled.
     *
     * @param cv Condition variable to wait on
     * @param mutex Mutex that must be held when calling this function
     * @param timeout Maximum time to wait
     *
     * @pre mutex is locked by the calling thread
     * @post mutex is locked by the calling thread
     *
     * @return true if signaled, false if timeout expired
     */
    virtual bool waitConditionVariableFor(
        ConditionVariableHandle cv,
        MutexHandle mutex,
        std::chrono::milliseconds timeout
    ) = 0;

    /**
     * @brief Wake one thread waiting on a condition variable.
     *
     * @param cv Condition variable to signal
     */
    virtual void notifyOne(ConditionVariableHandle cv) = 0;

    /**
     * @brief Wake all threads waiting on a condition variable.
     *
     * @param cv Condition variable to signal
     */
    virtual void notifyAll(ConditionVariableHandle cv) = 0;

    // =========================================================================
    // Current Thread Operations
    // =========================================================================

    /**
     * @brief Get the current thread's identifier.
     *
     * @return Unique identifier for the calling thread
     */
    virtual ThreadId getCurrentThreadId() = 0;

    /**
     * @brief Set the current thread's name.
     *
     * Sets a descriptive name for the calling thread. This name may
     * appear in debuggers and profiling tools.
     *
     * @param name Thread name (may be truncated on some platforms)
     *
     * @note On some platforms, the name is limited to 15-16 characters
     */
    virtual void setThreadName(const std::string& name) = 0;

    /**
     * @brief Sleep the current thread.
     *
     * Suspends execution of the calling thread for at least the
     * specified duration.
     *
     * @param duration Minimum time to sleep
     *
     * @note Actual sleep time may be longer due to scheduling
     */
    virtual void sleepFor(std::chrono::milliseconds duration) = 0;
};

} // namespace pal
} // namespace openrtmp

#endif // OPENRTMP_PAL_THREAD_PAL_HPP
