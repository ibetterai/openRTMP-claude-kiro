// OpenRTMP - Cross-platform RTMP Server
// Darwin (macOS/iOS) Thread PAL Implementation

#if defined(__APPLE__)

#include "openrtmp/pal/darwin/darwin_thread_pal.hpp"

#include <dispatch/dispatch.h>
#include <cerrno>
#include <cstring>
#include <thread>

namespace openrtmp {
namespace pal {
namespace darwin {

// Thread entry point wrapper data
struct ThreadEntryData {
    ThreadFunction func;
    void* arg;
    std::string name;
};

DarwinThreadPAL::DarwinThreadPAL() = default;

DarwinThreadPAL::~DarwinThreadPAL() {
    // Clean up any remaining mutexes
    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        for (auto& pair : mutexes_) {
            if (pair.second) {
                pthread_mutex_destroy(pair.second);
                delete pair.second;
            }
        }
        mutexes_.clear();
    }

    // Clean up any remaining condition variables
    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        for (auto& pair : conditionVariables_) {
            if (pair.second) {
                pthread_cond_destroy(pair.second);
                delete pair.second;
            }
        }
        conditionVariables_.clear();
    }

    // Clean up any remaining thread pools
    {
        std::lock_guard<std::mutex> lock(poolsMutex_);
        for (auto& pair : pools_) {
            if (pair.second.dispatchQueue) {
                dispatch_release(static_cast<dispatch_queue_t>(pair.second.dispatchQueue));
            }
        }
        pools_.clear();
    }
}

void* DarwinThreadPAL::threadEntryPoint(void* arg) {
    auto* data = static_cast<ThreadEntryData*>(arg);

    // Set thread name if provided
    if (!data->name.empty()) {
        pthread_setname_np(data->name.c_str());
    }

    // Call the user function
    data->func(data->arg);

    delete data;
    return nullptr;
}

// =============================================================================
// Thread Creation and Management
// =============================================================================

core::Result<ThreadHandle, ThreadError> DarwinThreadPAL::createThread(
    ThreadFunction func,
    void* arg,
    const ThreadOptions& options)
{
    pthread_attr_t attr;
    int result = pthread_attr_init(&attr);
    if (result != 0) {
        return core::Result<ThreadHandle, ThreadError>::error(
            ThreadError{ThreadErrorCode::CreationFailed, "Failed to initialize thread attributes"}
        );
    }

    // Set stack size if specified
    if (options.stackSize > 0) {
        result = pthread_attr_setstacksize(&attr, options.stackSize);
        if (result != 0) {
            pthread_attr_destroy(&attr);
            return core::Result<ThreadHandle, ThreadError>::error(
                ThreadError{ThreadErrorCode::CreationFailed, "Failed to set stack size"}
            );
        }
    }

    // Set detached state if requested
    if (options.detached) {
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    }

    // Create entry data
    auto* entryData = new ThreadEntryData{func, arg, options.name};

    // Create the thread
    pthread_t thread;
    result = pthread_create(&thread, &attr, threadEntryPoint, entryData);
    pthread_attr_destroy(&attr);

    if (result != 0) {
        delete entryData;
        ThreadErrorCode code = ThreadErrorCode::CreationFailed;
        if (result == EAGAIN) {
            code = ThreadErrorCode::ResourceExhausted;
        } else if (result == ENOMEM) {
            code = ThreadErrorCode::OutOfMemory;
        }
        return core::Result<ThreadHandle, ThreadError>::error(
            ThreadError{code, std::string("pthread_create failed: ") + strerror(result)}
        );
    }

    // Generate handle and store thread info
    uint64_t handleValue = generateHandle();
    ThreadHandle handle{handleValue};

    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        threads_[handleValue] = ThreadData{
            thread,
            func,
            arg,
            options.name,
            false,
            options.detached
        };
    }

    return core::Result<ThreadHandle, ThreadError>::success(handle);
}

core::Result<void, ThreadError> DarwinThreadPAL::joinThread(ThreadHandle handle) {
    pthread_t thread;

    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        auto it = threads_.find(handle.value);
        if (it == threads_.end()) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::InvalidHandle, "Thread not found"}
            );
        }

        if (it->second.joined) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::JoinFailed, "Thread already joined"}
            );
        }

        if (it->second.detached) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::JoinFailed, "Cannot join detached thread"}
            );
        }

        thread = it->second.thread;
    }

    int result = pthread_join(thread, nullptr);

    if (result != 0) {
        ThreadErrorCode code = ThreadErrorCode::JoinFailed;
        if (result == EDEADLK) {
            code = ThreadErrorCode::DeadlockDetected;
        }
        return core::Result<void, ThreadError>::error(
            ThreadError{code, std::string("pthread_join failed: ") + strerror(result)}
        );
    }

    // Mark as joined
    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        auto it = threads_.find(handle.value);
        if (it != threads_.end()) {
            it->second.joined = true;
        }
    }

    return core::Result<void, ThreadError>::success();
}

core::Result<void, ThreadError> DarwinThreadPAL::detachThread(ThreadHandle handle) {
    pthread_t thread;

    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        auto it = threads_.find(handle.value);
        if (it == threads_.end()) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::InvalidHandle, "Thread not found"}
            );
        }

        if (it->second.detached) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::DetachFailed, "Thread already detached"}
            );
        }

        thread = it->second.thread;
    }

    int result = pthread_detach(thread);

    if (result != 0) {
        return core::Result<void, ThreadError>::error(
            ThreadError{ThreadErrorCode::DetachFailed, std::string("pthread_detach failed: ") + strerror(result)}
        );
    }

    // Mark as detached
    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        auto it = threads_.find(handle.value);
        if (it != threads_.end()) {
            it->second.detached = true;
        }
    }

    return core::Result<void, ThreadError>::success();
}

// =============================================================================
// Thread Pool Operations (using GCD)
// =============================================================================

core::Result<ThreadPoolHandle, ThreadError> DarwinThreadPAL::createThreadPool(
    size_t minThreads,
    size_t maxThreads,
    const ThreadPoolOptions& options)
{
    // GCD manages thread count automatically, but we can influence behavior
    // Create a concurrent dispatch queue

    const char* label = options.name.empty() ?
        "com.openrtmp.threadpool" :
        options.name.c_str();

    dispatch_queue_t queue = dispatch_queue_create(label, DISPATCH_QUEUE_CONCURRENT);

    if (!queue) {
        return core::Result<ThreadPoolHandle, ThreadError>::error(
            ThreadError{ThreadErrorCode::PoolCreationFailed, "Failed to create dispatch queue"}
        );
    }

    uint64_t handleValue = generateHandle();
    ThreadPoolHandle handle{handleValue};

    {
        std::lock_guard<std::mutex> lock(poolsMutex_);
        pools_[handleValue] = ThreadPoolData{
            static_cast<void*>(queue),
            options.name,
            false
        };
    }

    return core::Result<ThreadPoolHandle, ThreadError>::success(handle);
}

core::Result<void, ThreadError> DarwinThreadPAL::submitWork(
    ThreadPoolHandle pool,
    WorkItem work)
{
    dispatch_queue_t queue = nullptr;

    {
        std::lock_guard<std::mutex> lock(poolsMutex_);
        auto it = pools_.find(pool.value);
        if (it == pools_.end()) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::InvalidHandle, "Thread pool not found"}
            );
        }

        if (it->second.shutdown) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::PoolShutdown, "Thread pool is shutting down"}
            );
        }

        queue = static_cast<dispatch_queue_t>(it->second.dispatchQueue);
    }

    // Wrap the work item for dispatch
    auto workCopy = std::make_shared<WorkItem>(std::move(work));

    dispatch_async(queue, ^{
        if (*workCopy) {
            (*workCopy)();
        }
    });

    return core::Result<void, ThreadError>::success();
}

core::Result<void, ThreadError> DarwinThreadPAL::destroyThreadPool(ThreadPoolHandle pool) {
    dispatch_queue_t queue = nullptr;

    {
        std::lock_guard<std::mutex> lock(poolsMutex_);
        auto it = pools_.find(pool.value);
        if (it == pools_.end()) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::InvalidHandle, "Thread pool not found"}
            );
        }

        it->second.shutdown = true;
        queue = static_cast<dispatch_queue_t>(it->second.dispatchQueue);
    }

    // Wait for all pending work to complete using a barrier
    dispatch_barrier_sync(queue, ^{
        // This block runs after all pending work is complete
    });

    // Release the queue
    dispatch_release(queue);

    // Remove from map
    {
        std::lock_guard<std::mutex> lock(poolsMutex_);
        pools_.erase(pool.value);
    }

    return core::Result<void, ThreadError>::success();
}

// =============================================================================
// Synchronization Primitives
// =============================================================================

core::Result<MutexHandle, ThreadError> DarwinThreadPAL::createMutex() {
    auto* mutex = new pthread_mutex_t;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_DEFAULT);

    int result = pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    if (result != 0) {
        delete mutex;
        return core::Result<MutexHandle, ThreadError>::error(
            ThreadError{ThreadErrorCode::MutexCreationFailed, std::string("pthread_mutex_init failed: ") + strerror(result)}
        );
    }

    uint64_t handleValue = generateHandle();
    MutexHandle handle{handleValue};

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        mutexes_[handleValue] = mutex;
    }

    return core::Result<MutexHandle, ThreadError>::success(handle);
}

void DarwinThreadPAL::destroyMutex(MutexHandle handle) {
    pthread_mutex_t* mutex = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        auto it = mutexes_.find(handle.value);
        if (it == mutexes_.end()) {
            return;
        }
        mutex = it->second;
        mutexes_.erase(it);
    }

    if (mutex) {
        pthread_mutex_destroy(mutex);
        delete mutex;
    }
}

void DarwinThreadPAL::lockMutex(MutexHandle handle) {
    pthread_mutex_t* mutex = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        auto it = mutexes_.find(handle.value);
        if (it != mutexes_.end()) {
            mutex = it->second;
        }
    }

    if (mutex) {
        pthread_mutex_lock(mutex);
    }
}

bool DarwinThreadPAL::tryLockMutex(MutexHandle handle) {
    pthread_mutex_t* mutex = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        auto it = mutexes_.find(handle.value);
        if (it != mutexes_.end()) {
            mutex = it->second;
        }
    }

    if (mutex) {
        return pthread_mutex_trylock(mutex) == 0;
    }

    return false;
}

void DarwinThreadPAL::unlockMutex(MutexHandle handle) {
    pthread_mutex_t* mutex = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        auto it = mutexes_.find(handle.value);
        if (it != mutexes_.end()) {
            mutex = it->second;
        }
    }

    if (mutex) {
        pthread_mutex_unlock(mutex);
    }
}

core::Result<ConditionVariableHandle, ThreadError> DarwinThreadPAL::createConditionVariable() {
    auto* cv = new pthread_cond_t;

    int result = pthread_cond_init(cv, nullptr);

    if (result != 0) {
        delete cv;
        return core::Result<ConditionVariableHandle, ThreadError>::error(
            ThreadError{ThreadErrorCode::ConditionVariableError, std::string("pthread_cond_init failed: ") + strerror(result)}
        );
    }

    uint64_t handleValue = generateHandle();
    ConditionVariableHandle handle{handleValue};

    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        conditionVariables_[handleValue] = cv;
    }

    return core::Result<ConditionVariableHandle, ThreadError>::success(handle);
}

void DarwinThreadPAL::destroyConditionVariable(ConditionVariableHandle handle) {
    pthread_cond_t* cv = nullptr;

    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        auto it = conditionVariables_.find(handle.value);
        if (it == conditionVariables_.end()) {
            return;
        }
        cv = it->second;
        conditionVariables_.erase(it);
    }

    if (cv) {
        pthread_cond_destroy(cv);
        delete cv;
    }
}

void DarwinThreadPAL::waitConditionVariable(ConditionVariableHandle cvHandle, MutexHandle mutexHandle) {
    pthread_cond_t* cv = nullptr;
    pthread_mutex_t* mutex = nullptr;

    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        auto it = conditionVariables_.find(cvHandle.value);
        if (it != conditionVariables_.end()) {
            cv = it->second;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        auto it = mutexes_.find(mutexHandle.value);
        if (it != mutexes_.end()) {
            mutex = it->second;
        }
    }

    if (cv && mutex) {
        pthread_cond_wait(cv, mutex);
    }
}

bool DarwinThreadPAL::waitConditionVariableFor(
    ConditionVariableHandle cvHandle,
    MutexHandle mutexHandle,
    std::chrono::milliseconds timeout)
{
    pthread_cond_t* cv = nullptr;
    pthread_mutex_t* mutex = nullptr;

    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        auto it = conditionVariables_.find(cvHandle.value);
        if (it != conditionVariables_.end()) {
            cv = it->second;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        auto it = mutexes_.find(mutexHandle.value);
        if (it != mutexes_.end()) {
            mutex = it->second;
        }
    }

    if (!cv || !mutex) {
        return false;
    }

    // Calculate absolute time
    struct timespec abstime;
    clock_gettime(CLOCK_REALTIME, &abstime);

    int64_t nanos = static_cast<int64_t>(abstime.tv_nsec) +
                    static_cast<int64_t>(timeout.count()) * 1000000LL;

    abstime.tv_sec += static_cast<time_t>(nanos / 1000000000LL);
    abstime.tv_nsec = static_cast<long>(nanos % 1000000000LL);

    int result = pthread_cond_timedwait(cv, mutex, &abstime);

    return result == 0;
}

void DarwinThreadPAL::notifyOne(ConditionVariableHandle handle) {
    pthread_cond_t* cv = nullptr;

    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        auto it = conditionVariables_.find(handle.value);
        if (it != conditionVariables_.end()) {
            cv = it->second;
        }
    }

    if (cv) {
        pthread_cond_signal(cv);
    }
}

void DarwinThreadPAL::notifyAll(ConditionVariableHandle handle) {
    pthread_cond_t* cv = nullptr;

    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        auto it = conditionVariables_.find(handle.value);
        if (it != conditionVariables_.end()) {
            cv = it->second;
        }
    }

    if (cv) {
        pthread_cond_broadcast(cv);
    }
}

// =============================================================================
// Current Thread Operations
// =============================================================================

ThreadId DarwinThreadPAL::getCurrentThreadId() {
    pthread_t self = pthread_self();
    // Convert pthread_t to uint64_t
    // On macOS, pthread_t is a pointer
    return ThreadId{reinterpret_cast<uint64_t>(self)};
}

void DarwinThreadPAL::setThreadName(const std::string& name) {
    // pthread_setname_np on Darwin only sets name for current thread
    // and has a 63 character limit
    std::string truncatedName = name.substr(0, 63);
    pthread_setname_np(truncatedName.c_str());
}

void DarwinThreadPAL::sleepFor(std::chrono::milliseconds duration) {
    std::this_thread::sleep_for(duration);
}

uint64_t DarwinThreadPAL::generateHandle() {
    return nextHandle_.fetch_add(1);
}

} // namespace darwin
} // namespace pal
} // namespace openrtmp

#endif // __APPLE__
