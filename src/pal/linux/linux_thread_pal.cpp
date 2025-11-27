// OpenRTMP - Cross-platform RTMP Server
// Linux/Android Thread PAL Implementation

#include "openrtmp/pal/linux/linux_thread_pal.hpp"

#if defined(__linux__) || defined(__ANDROID__)

#include <pthread.h>
#include <unistd.h>
#include <cstring>
#include <sys/prctl.h>
#include <errno.h>
#include <time.h>

namespace openrtmp {
namespace pal {
namespace linux {

// =============================================================================
// Thread Entry Point Wrapper
// =============================================================================

struct ThreadStartData {
    ThreadFunction func;
    void* arg;
    std::string name;
};

void* LinuxThreadPAL::threadEntryPoint(void* arg) {
    auto* startData = static_cast<ThreadStartData*>(arg);

    // Set thread name if provided
    if (!startData->name.empty()) {
        // Linux limits thread names to 16 characters including null terminator
        std::string truncatedName = startData->name.substr(0, 15);
#ifdef __ANDROID__
        pthread_setname_np(pthread_self(), truncatedName.c_str());
#else
        prctl(PR_SET_NAME, truncatedName.c_str(), 0, 0, 0);
#endif
    }

    // Call the actual thread function
    ThreadFunction func = std::move(startData->func);
    void* userArg = startData->arg;
    delete startData;

    func(userArg);

    return nullptr;
}

// =============================================================================
// Thread Pool Worker Function
// =============================================================================

void* LinuxThreadPAL::poolWorkerFunction(void* arg) {
    auto* pool = static_cast<ThreadPoolData*>(arg);

    while (true) {
        WorkItem work;

        {
            std::unique_lock<std::mutex> lock(pool->queueMutex);

            // Wait for work or shutdown
            pool->workCondition.wait(lock, [pool]() {
                return pool->shutdown.load() || !pool->workQueue.empty();
            });

            if (pool->shutdown.load() && pool->workQueue.empty()) {
                break;
            }

            if (!pool->workQueue.empty()) {
                work = std::move(pool->workQueue.front());
                pool->workQueue.pop();
            }
        }

        // Execute work item outside the lock
        if (work) {
            work();
        }
    }

    return nullptr;
}

// =============================================================================
// Constructor / Destructor
// =============================================================================

LinuxThreadPAL::LinuxThreadPAL() = default;

LinuxThreadPAL::~LinuxThreadPAL() {
    // Clean up remaining mutexes
    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        for (auto& pair : mutexes_) {
            pthread_mutex_destroy(pair.second);
            delete pair.second;
        }
        mutexes_.clear();
    }

    // Clean up remaining condition variables
    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        for (auto& pair : conditionVariables_) {
            pthread_cond_destroy(pair.second);
            delete pair.second;
        }
        conditionVariables_.clear();
    }
}

// =============================================================================
// Thread Creation and Management
// =============================================================================

uint64_t LinuxThreadPAL::generateHandle() {
    return nextHandle_.fetch_add(1, std::memory_order_relaxed);
}

core::Result<ThreadHandle, ThreadError> LinuxThreadPAL::createThread(
    ThreadFunction func,
    void* arg,
    const ThreadOptions& options
) {
    if (!func) {
        return core::Result<ThreadHandle, ThreadError>::error(
            ThreadError{ThreadErrorCode::CreationFailed, "Thread function is null"}
        );
    }

    auto startData = new ThreadStartData{std::move(func), arg, options.name};

    pthread_attr_t attr;
    pthread_attr_init(&attr);

    // Set stack size if specified
    if (options.stackSize > 0) {
        pthread_attr_setstacksize(&attr, options.stackSize);
    }

    // Set detached state if requested
    if (options.detached) {
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    }

    pthread_t thread;
    int result = pthread_create(&thread, &attr, threadEntryPoint, startData);
    pthread_attr_destroy(&attr);

    if (result != 0) {
        delete startData;
        return core::Result<ThreadHandle, ThreadError>::error(
            ThreadError{ThreadErrorCode::CreationFailed, "pthread_create failed: " + std::string(strerror(result))}
        );
    }

    uint64_t handleValue = generateHandle();
    ThreadHandle handle{handleValue};

    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        auto threadData = std::make_unique<ThreadData>();
        threadData->thread = thread;
        threadData->name = options.name;
        threadData->joined = false;
        threadData->detached = options.detached;
        threads_[handleValue] = std::move(threadData);
    }

    return core::Result<ThreadHandle, ThreadError>::success(handle);
}

core::Result<void, ThreadError> LinuxThreadPAL::joinThread(ThreadHandle handle) {
    pthread_t thread;

    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        auto it = threads_.find(handle.value);
        if (it == threads_.end()) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::InvalidHandle, "Invalid thread handle"}
            );
        }

        if (it->second->joined) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::JoinFailed, "Thread already joined"}
            );
        }

        if (it->second->detached) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::JoinFailed, "Cannot join detached thread"}
            );
        }

        thread = it->second->thread;
        it->second->joined = true;
    }

    int result = pthread_join(thread, nullptr);
    if (result != 0) {
        return core::Result<void, ThreadError>::error(
            ThreadError{ThreadErrorCode::JoinFailed, "pthread_join failed: " + std::string(strerror(result))}
        );
    }

    // Remove from map
    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        threads_.erase(handle.value);
    }

    return core::Result<void, ThreadError>::success();
}

core::Result<void, ThreadError> LinuxThreadPAL::detachThread(ThreadHandle handle) {
    pthread_t thread;

    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        auto it = threads_.find(handle.value);
        if (it == threads_.end()) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::InvalidHandle, "Invalid thread handle"}
            );
        }

        if (it->second->detached) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::DetachFailed, "Thread already detached"}
            );
        }

        thread = it->second->thread;
        it->second->detached = true;
    }

    int result = pthread_detach(thread);
    if (result != 0) {
        return core::Result<void, ThreadError>::error(
            ThreadError{ThreadErrorCode::DetachFailed, "pthread_detach failed: " + std::string(strerror(result))}
        );
    }

    // Remove from map
    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        threads_.erase(handle.value);
    }

    return core::Result<void, ThreadError>::success();
}

// =============================================================================
// Thread Pool Operations
// =============================================================================

core::Result<ThreadPoolHandle, ThreadError> LinuxThreadPAL::createThreadPool(
    size_t minThreads,
    size_t maxThreads,
    const ThreadPoolOptions& options
) {
    if (minThreads == 0 || maxThreads < minThreads) {
        return core::Result<ThreadPoolHandle, ThreadError>::error(
            ThreadError{ThreadErrorCode::PoolCreationFailed, "Invalid thread count"}
        );
    }

    uint64_t handleValue = generateHandle();
    auto poolData = std::make_unique<ThreadPoolData>();
    poolData->name = options.name;
    poolData->minThreads = minThreads;
    poolData->maxThreads = maxThreads;

    // Create worker threads
    for (size_t i = 0; i < minThreads; ++i) {
        pthread_t worker;
        int result = pthread_create(&worker, nullptr, poolWorkerFunction, poolData.get());
        if (result != 0) {
            // Shutdown already created workers
            poolData->shutdown = true;
            poolData->workCondition.notify_all();
            for (auto& w : poolData->workers) {
                pthread_join(w, nullptr);
            }
            return core::Result<ThreadPoolHandle, ThreadError>::error(
                ThreadError{ThreadErrorCode::PoolCreationFailed, "Failed to create worker thread"}
            );
        }
        poolData->workers.push_back(worker);
    }

    {
        std::lock_guard<std::mutex> lock(poolsMutex_);
        pools_[handleValue] = std::move(poolData);
    }

    return core::Result<ThreadPoolHandle, ThreadError>::success(ThreadPoolHandle{handleValue});
}

core::Result<void, ThreadError> LinuxThreadPAL::submitWork(
    ThreadPoolHandle pool,
    WorkItem work
) {
    if (!work) {
        return core::Result<void, ThreadError>::error(
            ThreadError{ThreadErrorCode::Unknown, "Work item is null"}
        );
    }

    ThreadPoolData* poolData = nullptr;
    {
        std::lock_guard<std::mutex> lock(poolsMutex_);
        auto it = pools_.find(pool.value);
        if (it == pools_.end()) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::InvalidHandle, "Invalid thread pool handle"}
            );
        }
        poolData = it->second.get();
    }

    if (poolData->shutdown.load()) {
        return core::Result<void, ThreadError>::error(
            ThreadError{ThreadErrorCode::PoolShutdown, "Thread pool is shutting down"}
        );
    }

    {
        std::lock_guard<std::mutex> lock(poolData->queueMutex);
        poolData->workQueue.push(std::move(work));
    }
    poolData->workCondition.notify_one();

    return core::Result<void, ThreadError>::success();
}

core::Result<void, ThreadError> LinuxThreadPAL::destroyThreadPool(
    ThreadPoolHandle pool
) {
    std::unique_ptr<ThreadPoolData> poolData;

    {
        std::lock_guard<std::mutex> lock(poolsMutex_);
        auto it = pools_.find(pool.value);
        if (it == pools_.end()) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::InvalidHandle, "Invalid thread pool handle"}
            );
        }
        poolData = std::move(it->second);
        pools_.erase(it);
    }

    // Signal shutdown
    poolData->shutdown = true;
    poolData->workCondition.notify_all();

    // Wait for all workers to finish
    for (auto& worker : poolData->workers) {
        pthread_join(worker, nullptr);
    }

    return core::Result<void, ThreadError>::success();
}

// =============================================================================
// Synchronization Primitives
// =============================================================================

core::Result<MutexHandle, ThreadError> LinuxThreadPAL::createMutex() {
    auto* mutex = new pthread_mutex_t;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);

    int result = pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    if (result != 0) {
        delete mutex;
        return core::Result<MutexHandle, ThreadError>::error(
            ThreadError{ThreadErrorCode::MutexCreationFailed, "pthread_mutex_init failed"}
        );
    }

    uint64_t handleValue = generateHandle();

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        mutexes_[handleValue] = mutex;
    }

    return core::Result<MutexHandle, ThreadError>::success(MutexHandle{handleValue});
}

void LinuxThreadPAL::destroyMutex(MutexHandle handle) {
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

    pthread_mutex_destroy(mutex);
    delete mutex;
}

void LinuxThreadPAL::lockMutex(MutexHandle handle) {
    pthread_mutex_t* mutex = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        auto it = mutexes_.find(handle.value);
        if (it == mutexes_.end()) {
            return;
        }
        mutex = it->second;
    }

    pthread_mutex_lock(mutex);
}

bool LinuxThreadPAL::tryLockMutex(MutexHandle handle) {
    pthread_mutex_t* mutex = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        auto it = mutexes_.find(handle.value);
        if (it == mutexes_.end()) {
            return false;
        }
        mutex = it->second;
    }

    return pthread_mutex_trylock(mutex) == 0;
}

void LinuxThreadPAL::unlockMutex(MutexHandle handle) {
    pthread_mutex_t* mutex = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        auto it = mutexes_.find(handle.value);
        if (it == mutexes_.end()) {
            return;
        }
        mutex = it->second;
    }

    pthread_mutex_unlock(mutex);
}

core::Result<ConditionVariableHandle, ThreadError> LinuxThreadPAL::createConditionVariable() {
    auto* cv = new pthread_cond_t;

    int result = pthread_cond_init(cv, nullptr);
    if (result != 0) {
        delete cv;
        return core::Result<ConditionVariableHandle, ThreadError>::error(
            ThreadError{ThreadErrorCode::ConditionVariableError, "pthread_cond_init failed"}
        );
    }

    uint64_t handleValue = generateHandle();

    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        conditionVariables_[handleValue] = cv;
    }

    return core::Result<ConditionVariableHandle, ThreadError>::success(ConditionVariableHandle{handleValue});
}

void LinuxThreadPAL::destroyConditionVariable(ConditionVariableHandle handle) {
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

    pthread_cond_destroy(cv);
    delete cv;
}

void LinuxThreadPAL::waitConditionVariable(ConditionVariableHandle cv, MutexHandle mutex) {
    pthread_cond_t* cvPtr = nullptr;
    pthread_mutex_t* mutexPtr = nullptr;

    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        auto it = conditionVariables_.find(cv.value);
        if (it == conditionVariables_.end()) {
            return;
        }
        cvPtr = it->second;
    }

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        auto it = mutexes_.find(mutex.value);
        if (it == mutexes_.end()) {
            return;
        }
        mutexPtr = it->second;
    }

    pthread_cond_wait(cvPtr, mutexPtr);
}

bool LinuxThreadPAL::waitConditionVariableFor(
    ConditionVariableHandle cv,
    MutexHandle mutex,
    std::chrono::milliseconds timeout
) {
    pthread_cond_t* cvPtr = nullptr;
    pthread_mutex_t* mutexPtr = nullptr;

    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        auto it = conditionVariables_.find(cv.value);
        if (it == conditionVariables_.end()) {
            return false;
        }
        cvPtr = it->second;
    }

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        auto it = mutexes_.find(mutex.value);
        if (it == mutexes_.end()) {
            return false;
        }
        mutexPtr = it->second;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    int64_t nsec = ts.tv_nsec + (timeout.count() % 1000) * 1000000;
    ts.tv_sec += timeout.count() / 1000 + nsec / 1000000000;
    ts.tv_nsec = nsec % 1000000000;

    int result = pthread_cond_timedwait(cvPtr, mutexPtr, &ts);
    return result == 0;
}

void LinuxThreadPAL::notifyOne(ConditionVariableHandle cv) {
    pthread_cond_t* cvPtr = nullptr;

    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        auto it = conditionVariables_.find(cv.value);
        if (it == conditionVariables_.end()) {
            return;
        }
        cvPtr = it->second;
    }

    pthread_cond_signal(cvPtr);
}

void LinuxThreadPAL::notifyAll(ConditionVariableHandle cv) {
    pthread_cond_t* cvPtr = nullptr;

    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        auto it = conditionVariables_.find(cv.value);
        if (it == conditionVariables_.end()) {
            return;
        }
        cvPtr = it->second;
    }

    pthread_cond_broadcast(cvPtr);
}

// =============================================================================
// Current Thread Operations
// =============================================================================

ThreadId LinuxThreadPAL::getCurrentThreadId() {
    return ThreadId{static_cast<uint64_t>(pthread_self())};
}

void LinuxThreadPAL::setThreadName(const std::string& name) {
    std::string truncatedName = name.substr(0, 15);  // Linux limit
#ifdef __ANDROID__
    pthread_setname_np(pthread_self(), truncatedName.c_str());
#else
    prctl(PR_SET_NAME, truncatedName.c_str(), 0, 0, 0);
#endif
}

void LinuxThreadPAL::sleepFor(std::chrono::milliseconds duration) {
    usleep(static_cast<useconds_t>(duration.count() * 1000));
}

} // namespace linux
} // namespace pal
} // namespace openrtmp

#endif // defined(__linux__) || defined(__ANDROID__)
