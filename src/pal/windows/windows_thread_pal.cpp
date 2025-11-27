// OpenRTMP - Cross-platform RTMP Server
// Windows Thread PAL Implementation

#if defined(_WIN32)

#include "openrtmp/pal/windows/windows_thread_pal.hpp"

#include <cstring>

namespace openrtmp {
namespace pal {
namespace windows {

// =============================================================================
// WindowsThreadPAL Implementation
// =============================================================================

WindowsThreadPAL::WindowsThreadPAL() = default;

WindowsThreadPAL::~WindowsThreadPAL() {
    // Clean up any remaining resources
    // Note: We don't auto-join threads as the interface doc says caller is responsible

    // Clean up mutexes
    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        for (auto& pair : mutexes_) {
            DeleteCriticalSection(pair.second);
            delete pair.second;
        }
        mutexes_.clear();
    }

    // Clean up condition variables
    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        for (auto& pair : conditionVariables_) {
            delete pair.second;
        }
        conditionVariables_.clear();
    }
}

uint64_t WindowsThreadPAL::generateHandle() {
    return nextHandle_.fetch_add(1);
}

DWORD WINAPI WindowsThreadPAL::threadEntryPoint(LPVOID arg) {
    auto* data = static_cast<ThreadData*>(arg);

    // Set thread name if provided
    if (!data->name.empty()) {
        // SetThreadDescription is available on Windows 10 version 1607+
        typedef HRESULT (WINAPI *SetThreadDescriptionFn)(HANDLE, PCWSTR);
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (kernel32) {
            auto setThreadDescription = reinterpret_cast<SetThreadDescriptionFn>(
                GetProcAddress(kernel32, "SetThreadDescription"));
            if (setThreadDescription) {
                // Convert to wide string
                int wideLen = MultiByteToWideChar(CP_UTF8, 0, data->name.c_str(),
                                                   static_cast<int>(data->name.length()),
                                                   nullptr, 0);
                if (wideLen > 0) {
                    std::wstring wideName(static_cast<size_t>(wideLen), L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, data->name.c_str(),
                                       static_cast<int>(data->name.length()),
                                       &wideName[0], wideLen);
                    setThreadDescription(GetCurrentThread(), wideName.c_str());
                }
            }
        }
    }

    // Execute the thread function
    if (data->func) {
        data->func(data->arg);
    }

    return 0;
}

core::Result<ThreadHandle, ThreadError> WindowsThreadPAL::createThread(
    ThreadFunction func,
    void* arg,
    const ThreadOptions& options)
{
    uint64_t id = generateHandle();

    ThreadData data;
    data.func = func;
    data.arg = arg;
    data.name = options.name;
    data.joined = false;
    data.detached = false;

    // Store thread data first (will be accessed by thread entry point)
    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        threads_[id] = data;
    }

    // Create thread
    DWORD threadId;
    HANDLE handle = CreateThread(
        nullptr,  // default security
        options.stackSize,
        threadEntryPoint,
        &threads_[id],  // pass pointer to stored data
        0,  // run immediately
        &threadId
    );

    if (handle == nullptr) {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        threads_.erase(id);
        return core::Result<ThreadHandle, ThreadError>::error(
            ThreadError{ThreadErrorCode::CreationFailed,
                       "Failed to create thread",
                       static_cast<int>(GetLastError())}
        );
    }

    // Update thread data with handle
    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        threads_[id].handle = handle;
    }

    return core::Result<ThreadHandle, ThreadError>::success(ThreadHandle{id});
}

core::Result<void, ThreadError> WindowsThreadPAL::joinThread(ThreadHandle handle) {
    HANDLE winHandle;
    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        auto it = threads_.find(handle.value);
        if (it == threads_.end()) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::InvalidHandle, "Invalid thread handle", 0}
            );
        }

        if (it->second.joined || it->second.detached) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::InvalidHandle,
                           "Thread already joined or detached", 0}
            );
        }

        winHandle = it->second.handle;
    }

    // Wait for thread to complete
    DWORD result = WaitForSingleObject(winHandle, INFINITE);
    if (result != WAIT_OBJECT_0) {
        return core::Result<void, ThreadError>::error(
            ThreadError{ThreadErrorCode::Unknown,
                       "Failed to join thread",
                       static_cast<int>(GetLastError())}
        );
    }

    // Close handle and mark as joined
    CloseHandle(winHandle);

    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        auto it = threads_.find(handle.value);
        if (it != threads_.end()) {
            it->second.joined = true;
        }
    }

    return core::Result<void, ThreadError>::success();
}

core::Result<void, ThreadError> WindowsThreadPAL::detachThread(ThreadHandle handle) {
    HANDLE winHandle;
    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        auto it = threads_.find(handle.value);
        if (it == threads_.end()) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::InvalidHandle, "Invalid thread handle", 0}
            );
        }

        if (it->second.joined || it->second.detached) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::InvalidHandle,
                           "Thread already joined or detached", 0}
            );
        }

        winHandle = it->second.handle;
        it->second.detached = true;
    }

    // Close handle to allow thread to clean up automatically
    CloseHandle(winHandle);

    return core::Result<void, ThreadError>::success();
}

// =============================================================================
// Thread Pool Implementation
// =============================================================================

VOID CALLBACK WindowsThreadPAL::workCallback(
    PTP_CALLBACK_INSTANCE instance,
    PVOID context,
    PTP_WORK work)
{
    (void)instance;

    auto* data = static_cast<WorkItemData*>(context);
    if (data && data->work) {
        data->work();
    }
}

core::Result<ThreadPoolHandle, ThreadError> WindowsThreadPAL::createThreadPool(
    size_t minThreads,
    size_t maxThreads,
    const ThreadPoolOptions& options)
{
    // Create thread pool
    PTP_POOL pool = CreateThreadpool(nullptr);
    if (pool == nullptr) {
        return core::Result<ThreadPoolHandle, ThreadError>::error(
            ThreadError{ThreadErrorCode::CreationFailed,
                       "Failed to create thread pool",
                       static_cast<int>(GetLastError())}
        );
    }

    // Set min/max threads
    SetThreadpoolThreadMinimum(pool, static_cast<DWORD>(minThreads));
    SetThreadpoolThreadMaximum(pool, static_cast<DWORD>(maxThreads));

    // Create cleanup group
    PTP_CLEANUP_GROUP cleanupGroup = CreateThreadpoolCleanupGroup();
    if (cleanupGroup == nullptr) {
        CloseThreadpool(pool);
        return core::Result<ThreadPoolHandle, ThreadError>::error(
            ThreadError{ThreadErrorCode::CreationFailed,
                       "Failed to create cleanup group",
                       static_cast<int>(GetLastError())}
        );
    }

    // Initialize callback environment
    ThreadPoolData data;
    InitializeThreadpoolEnvironment(&data.callbackEnv);
    SetThreadpoolCallbackPool(&data.callbackEnv, pool);
    SetThreadpoolCallbackCleanupGroup(&data.callbackEnv, cleanupGroup, nullptr);

    data.pool = pool;
    data.cleanupGroup = cleanupGroup;
    data.name = options.name;
    data.shutdown = false;

    uint64_t id = generateHandle();

    {
        std::lock_guard<std::mutex> lock(poolsMutex_);
        pools_[id] = data;
    }

    return core::Result<ThreadPoolHandle, ThreadError>::success(ThreadPoolHandle{id});
}

core::Result<void, ThreadError> WindowsThreadPAL::submitWork(
    ThreadPoolHandle pool,
    WorkItem work)
{
    ThreadPoolData* poolData = nullptr;
    {
        std::lock_guard<std::mutex> lock(poolsMutex_);
        auto it = pools_.find(pool.value);
        if (it == pools_.end()) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::InvalidHandle, "Invalid pool handle", 0}
            );
        }

        if (it->second.shutdown) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::PoolShutdown, "Pool is shutting down", 0}
            );
        }

        poolData = &it->second;
    }

    // Create work item data
    auto workData = std::make_unique<WorkItemData>();
    workData->work = std::move(work);

    // Create work item
    PTP_WORK tpWork = CreateThreadpoolWork(
        workCallback,
        workData.get(),
        &poolData->callbackEnv
    );

    if (tpWork == nullptr) {
        return core::Result<void, ThreadError>::error(
            ThreadError{ThreadErrorCode::Unknown,
                       "Failed to create work item",
                       static_cast<int>(GetLastError())}
        );
    }

    workData->tpWork = tpWork;

    // Store work item
    {
        std::lock_guard<std::mutex> lock(workItemsMutex_);
        workItems_[tpWork] = std::move(workData);
    }

    // Submit work
    SubmitThreadpoolWork(tpWork);

    return core::Result<void, ThreadError>::success();
}

core::Result<void, ThreadError> WindowsThreadPAL::destroyThreadPool(ThreadPoolHandle pool) {
    ThreadPoolData data;
    {
        std::lock_guard<std::mutex> lock(poolsMutex_);
        auto it = pools_.find(pool.value);
        if (it == pools_.end()) {
            return core::Result<void, ThreadError>::error(
                ThreadError{ThreadErrorCode::InvalidHandle, "Invalid pool handle", 0}
            );
        }

        data = it->second;
        it->second.shutdown = true;
    }

    // Wait for all work to complete and clean up
    CloseThreadpoolCleanupGroupMembers(data.cleanupGroup, FALSE, nullptr);
    CloseThreadpoolCleanupGroup(data.cleanupGroup);
    DestroyThreadpoolEnvironment(&data.callbackEnv);
    CloseThreadpool(data.pool);

    // Remove from map
    {
        std::lock_guard<std::mutex> lock(poolsMutex_);
        pools_.erase(pool.value);
    }

    return core::Result<void, ThreadError>::success();
}

// =============================================================================
// Mutex Implementation
// =============================================================================

core::Result<MutexHandle, ThreadError> WindowsThreadPAL::createMutex() {
    auto* cs = new CRITICAL_SECTION;
    InitializeCriticalSection(cs);

    uint64_t id = generateHandle();

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        mutexes_[id] = cs;
    }

    return core::Result<MutexHandle, ThreadError>::success(MutexHandle{id});
}

void WindowsThreadPAL::destroyMutex(MutexHandle handle) {
    CRITICAL_SECTION* cs = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        auto it = mutexes_.find(handle.value);
        if (it != mutexes_.end()) {
            cs = it->second;
            mutexes_.erase(it);
        }
    }

    if (cs) {
        DeleteCriticalSection(cs);
        delete cs;
    }
}

void WindowsThreadPAL::lockMutex(MutexHandle handle) {
    CRITICAL_SECTION* cs = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        auto it = mutexes_.find(handle.value);
        if (it != mutexes_.end()) {
            cs = it->second;
        }
    }

    if (cs) {
        EnterCriticalSection(cs);
    }
}

bool WindowsThreadPAL::tryLockMutex(MutexHandle handle) {
    CRITICAL_SECTION* cs = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        auto it = mutexes_.find(handle.value);
        if (it != mutexes_.end()) {
            cs = it->second;
        }
    }

    if (cs) {
        return TryEnterCriticalSection(cs) != FALSE;
    }

    return false;
}

void WindowsThreadPAL::unlockMutex(MutexHandle handle) {
    CRITICAL_SECTION* cs = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        auto it = mutexes_.find(handle.value);
        if (it != mutexes_.end()) {
            cs = it->second;
        }
    }

    if (cs) {
        LeaveCriticalSection(cs);
    }
}

// =============================================================================
// Condition Variable Implementation
// =============================================================================

core::Result<ConditionVariableHandle, ThreadError> WindowsThreadPAL::createConditionVariable() {
    auto* cv = new CONDITION_VARIABLE;
    InitializeConditionVariable(cv);

    uint64_t id = generateHandle();

    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        conditionVariables_[id] = cv;
    }

    return core::Result<ConditionVariableHandle, ThreadError>::success(
        ConditionVariableHandle{id}
    );
}

void WindowsThreadPAL::destroyConditionVariable(ConditionVariableHandle handle) {
    CONDITION_VARIABLE* cv = nullptr;

    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        auto it = conditionVariables_.find(handle.value);
        if (it != conditionVariables_.end()) {
            cv = it->second;
            conditionVariables_.erase(it);
        }
    }

    if (cv) {
        // No cleanup needed for Windows CONDITION_VARIABLE
        delete cv;
    }
}

void WindowsThreadPAL::waitConditionVariable(ConditionVariableHandle cv, MutexHandle mutex) {
    CONDITION_VARIABLE* winCv = nullptr;
    CRITICAL_SECTION* cs = nullptr;

    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        auto it = conditionVariables_.find(cv.value);
        if (it != conditionVariables_.end()) {
            winCv = it->second;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        auto it = mutexes_.find(mutex.value);
        if (it != mutexes_.end()) {
            cs = it->second;
        }
    }

    if (winCv && cs) {
        SleepConditionVariableCS(winCv, cs, INFINITE);
    }
}

bool WindowsThreadPAL::waitConditionVariableFor(
    ConditionVariableHandle cv,
    MutexHandle mutex,
    std::chrono::milliseconds timeout)
{
    CONDITION_VARIABLE* winCv = nullptr;
    CRITICAL_SECTION* cs = nullptr;

    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        auto it = conditionVariables_.find(cv.value);
        if (it != conditionVariables_.end()) {
            winCv = it->second;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutexesMutex_);
        auto it = mutexes_.find(mutex.value);
        if (it != mutexes_.end()) {
            cs = it->second;
        }
    }

    if (winCv && cs) {
        return SleepConditionVariableCS(winCv, cs,
            static_cast<DWORD>(timeout.count())) != FALSE;
    }

    return false;
}

void WindowsThreadPAL::notifyOne(ConditionVariableHandle cv) {
    CONDITION_VARIABLE* winCv = nullptr;

    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        auto it = conditionVariables_.find(cv.value);
        if (it != conditionVariables_.end()) {
            winCv = it->second;
        }
    }

    if (winCv) {
        WakeConditionVariable(winCv);
    }
}

void WindowsThreadPAL::notifyAll(ConditionVariableHandle cv) {
    CONDITION_VARIABLE* winCv = nullptr;

    {
        std::lock_guard<std::mutex> lock(cvsMutex_);
        auto it = conditionVariables_.find(cv.value);
        if (it != conditionVariables_.end()) {
            winCv = it->second;
        }
    }

    if (winCv) {
        WakeAllConditionVariable(winCv);
    }
}

// =============================================================================
// Current Thread Operations
// =============================================================================

ThreadId WindowsThreadPAL::getCurrentThreadId() {
    return ThreadId{static_cast<uint64_t>(GetCurrentThreadId())};
}

void WindowsThreadPAL::setThreadName(const std::string& name) {
    // SetThreadDescription is available on Windows 10 version 1607+
    typedef HRESULT (WINAPI *SetThreadDescriptionFn)(HANDLE, PCWSTR);
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32) {
        auto setThreadDescription = reinterpret_cast<SetThreadDescriptionFn>(
            GetProcAddress(kernel32, "SetThreadDescription"));
        if (setThreadDescription) {
            // Convert to wide string
            int wideLen = MultiByteToWideChar(CP_UTF8, 0, name.c_str(),
                                               static_cast<int>(name.length()),
                                               nullptr, 0);
            if (wideLen > 0) {
                std::wstring wideName(static_cast<size_t>(wideLen), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, name.c_str(),
                                   static_cast<int>(name.length()),
                                   &wideName[0], wideLen);
                setThreadDescription(GetCurrentThread(), wideName.c_str());
            }
        }
    }
}

void WindowsThreadPAL::sleepFor(std::chrono::milliseconds duration) {
    Sleep(static_cast<DWORD>(duration.count()));
}

} // namespace windows
} // namespace pal
} // namespace openrtmp

#endif // _WIN32
