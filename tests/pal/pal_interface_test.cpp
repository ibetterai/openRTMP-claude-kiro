// OpenRTMP - Cross-platform RTMP Server
// Tests for Platform Abstraction Layer (PAL) interfaces
//
// These tests verify:
// 1. Interface definitions compile correctly
// 2. Mock implementations satisfy interface contracts
// 3. Callback types are correctly defined
// 4. Error types are properly structured

#include <gtest/gtest.h>
#include "openrtmp/pal/pal_types.hpp"
#include "openrtmp/pal/network_pal.hpp"
#include "openrtmp/pal/thread_pal.hpp"
#include "openrtmp/pal/timer_pal.hpp"
#include "openrtmp/pal/file_pal.hpp"
#include "openrtmp/pal/log_pal.hpp"
#include "openrtmp/core/result.hpp"
#include "openrtmp/core/buffer.hpp"

namespace openrtmp {
namespace pal {
namespace test {

// =============================================================================
// PAL Types Tests
// =============================================================================

TEST(PALTypesTest, SocketHandleHasInvalidConstant) {
    EXPECT_NE(INVALID_SOCKET_HANDLE, SocketHandle{1});
}

TEST(PALTypesTest, ThreadHandleHasInvalidConstant) {
    EXPECT_NE(INVALID_THREAD_HANDLE, ThreadHandle{1});
}

TEST(PALTypesTest, TimerHandleHasInvalidConstant) {
    EXPECT_NE(INVALID_TIMER_HANDLE, TimerHandle{1});
}

TEST(PALTypesTest, FileHandleHasInvalidConstant) {
    EXPECT_NE(INVALID_FILE_HANDLE, FileHandle{1});
}

TEST(PALTypesTest, NetworkErrorCodesCoverage) {
    // Verify all network error codes are distinct
    EXPECT_NE(static_cast<int>(NetworkErrorCode::Success),
              static_cast<int>(NetworkErrorCode::Unknown));
    EXPECT_NE(static_cast<int>(NetworkErrorCode::BindFailed),
              static_cast<int>(NetworkErrorCode::ConnectionFailed));
    EXPECT_NE(static_cast<int>(NetworkErrorCode::WouldBlock),
              static_cast<int>(NetworkErrorCode::Timeout));
}

TEST(PALTypesTest, ThreadErrorCodesCoverage) {
    EXPECT_NE(static_cast<int>(ThreadErrorCode::Success),
              static_cast<int>(ThreadErrorCode::CreationFailed));
    EXPECT_NE(static_cast<int>(ThreadErrorCode::InvalidHandle),
              static_cast<int>(ThreadErrorCode::ResourceExhausted));
}

TEST(PALTypesTest, FileErrorCodesCoverage) {
    EXPECT_NE(static_cast<int>(FileErrorCode::Success),
              static_cast<int>(FileErrorCode::NotFound));
    EXPECT_NE(static_cast<int>(FileErrorCode::PermissionDenied),
              static_cast<int>(FileErrorCode::IOError));
}

TEST(PALTypesTest, LogLevelOrdering) {
    // Log levels should be ordered from most verbose to least
    EXPECT_LT(static_cast<int>(LogLevel::Trace),
              static_cast<int>(LogLevel::Debug));
    EXPECT_LT(static_cast<int>(LogLevel::Debug),
              static_cast<int>(LogLevel::Info));
    EXPECT_LT(static_cast<int>(LogLevel::Info),
              static_cast<int>(LogLevel::Warning));
    EXPECT_LT(static_cast<int>(LogLevel::Warning),
              static_cast<int>(LogLevel::Error));
    EXPECT_LT(static_cast<int>(LogLevel::Error),
              static_cast<int>(LogLevel::Critical));
}

TEST(PALTypesTest, SocketOptionValues) {
    EXPECT_NE(static_cast<int>(SocketOption::NoDelay),
              static_cast<int>(SocketOption::ReuseAddr));
    EXPECT_NE(static_cast<int>(SocketOption::KeepAlive),
              static_cast<int>(SocketOption::SendBufferSize));
}

TEST(PALTypesTest, NetworkErrorConstruction) {
    NetworkError err{NetworkErrorCode::BindFailed, "Port already in use", 1935};

    EXPECT_EQ(err.code, NetworkErrorCode::BindFailed);
    EXPECT_EQ(err.message, "Port already in use");
    EXPECT_EQ(err.systemErrorCode, 1935);
}

TEST(PALTypesTest, ThreadErrorConstruction) {
    ThreadError err{ThreadErrorCode::CreationFailed, "Resource limit reached"};

    EXPECT_EQ(err.code, ThreadErrorCode::CreationFailed);
    EXPECT_EQ(err.message, "Resource limit reached");
}

TEST(PALTypesTest, FileErrorConstruction) {
    FileError err{FileErrorCode::NotFound, "/path/to/file.txt"};

    EXPECT_EQ(err.code, FileErrorCode::NotFound);
    EXPECT_EQ(err.path, "/path/to/file.txt");
}

TEST(PALTypesTest, ServerOptionsDefaults) {
    ServerOptions opts;

    EXPECT_GT(opts.backlog, 0);
    EXPECT_TRUE(opts.reuseAddr);
}

TEST(PALTypesTest, ThreadOptionsDefaults) {
    ThreadOptions opts;

    // Stack size should be reasonable default or 0 for system default
    EXPECT_GE(opts.stackSize, 0u);
}

// =============================================================================
// Network PAL Interface Tests
// =============================================================================

// Mock implementation for testing interface compliance
class MockNetworkPAL : public INetworkPAL {
public:
    // Event loop
    core::Result<void, NetworkError> initialize() override {
        initialized_ = true;
        return core::Result<void, NetworkError>::success();
    }

    void runEventLoop() override {
        running_ = true;
    }

    void stopEventLoop() override {
        running_ = false;
    }

    bool isRunning() const override {
        return running_;
    }

    // Server socket
    core::Result<ServerSocket, NetworkError> createServer(
        const std::string& address,
        uint16_t port,
        const ServerOptions& options
    ) override {
        ServerSocket server;
        server.handle = SocketHandle{100};
        server.address = address;
        server.port = port;
        return core::Result<ServerSocket, NetworkError>::success(server);
    }

    // Async operations
    void asyncAccept(
        ServerSocket& server,
        AcceptCallback callback
    ) override {
        acceptCallbackCalled_ = true;
    }

    void asyncRead(
        SocketHandle socket,
        core::Buffer& buffer,
        size_t maxBytes,
        ReadCallback callback
    ) override {
        readCallbackCalled_ = true;
    }

    void asyncWrite(
        SocketHandle socket,
        const core::Buffer& data,
        WriteCallback callback
    ) override {
        writeCallbackCalled_ = true;
    }

    // Socket management
    void closeSocket(SocketHandle socket) override {
        closedSocket_ = socket;
    }

    core::Result<void, NetworkError> setSocketOption(
        SocketHandle socket,
        SocketOption option,
        int value
    ) override {
        lastOption_ = option;
        lastOptionValue_ = value;
        return core::Result<void, NetworkError>::success();
    }

    core::Result<std::string, NetworkError> getLocalAddress(
        SocketHandle socket
    ) const override {
        return core::Result<std::string, NetworkError>::success("127.0.0.1");
    }

    core::Result<uint16_t, NetworkError> getLocalPort(
        SocketHandle socket
    ) const override {
        return core::Result<uint16_t, NetworkError>::success(1935);
    }

    // Test accessors
    bool wasInitialized() const { return initialized_; }
    bool wasAcceptCalled() const { return acceptCallbackCalled_; }
    bool wasReadCalled() const { return readCallbackCalled_; }
    bool wasWriteCalled() const { return writeCallbackCalled_; }
    SocketHandle getClosedSocket() const { return closedSocket_; }
    SocketOption getLastOption() const { return lastOption_; }
    int getLastOptionValue() const { return lastOptionValue_; }

private:
    bool initialized_ = false;
    bool running_ = false;
    bool acceptCallbackCalled_ = false;
    bool readCallbackCalled_ = false;
    bool writeCallbackCalled_ = false;
    SocketHandle closedSocket_ = INVALID_SOCKET_HANDLE;
    SocketOption lastOption_ = SocketOption::NoDelay;
    int lastOptionValue_ = 0;
};

TEST(NetworkPALTest, InterfaceCompiles) {
    // If this test compiles, the interface is defined correctly
    MockNetworkPAL pal;
    INetworkPAL* interface = &pal;
    EXPECT_NE(interface, nullptr);
}

TEST(NetworkPALTest, InitializeReturnsSuccess) {
    MockNetworkPAL pal;
    auto result = pal.initialize();

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(pal.wasInitialized());
}

TEST(NetworkPALTest, CreateServerReturnsValidSocket) {
    MockNetworkPAL pal;
    auto result = pal.createServer("0.0.0.0", 1935, ServerOptions{});

    EXPECT_TRUE(result.isSuccess());
    EXPECT_NE(result.value().handle, INVALID_SOCKET_HANDLE);
    EXPECT_EQ(result.value().port, 1935);
}

TEST(NetworkPALTest, AsyncAcceptRegistersCallback) {
    MockNetworkPAL pal;
    ServerSocket server;
    server.handle = SocketHandle{1};

    pal.asyncAccept(server, [](core::Result<SocketHandle, NetworkError>) {});

    EXPECT_TRUE(pal.wasAcceptCalled());
}

TEST(NetworkPALTest, AsyncReadRegistersCallback) {
    MockNetworkPAL pal;
    core::Buffer buffer;

    pal.asyncRead(SocketHandle{1}, buffer, 1024, [](core::Result<size_t, NetworkError>) {});

    EXPECT_TRUE(pal.wasReadCalled());
}

TEST(NetworkPALTest, AsyncWriteRegistersCallback) {
    MockNetworkPAL pal;
    core::Buffer data{0x01, 0x02, 0x03};

    pal.asyncWrite(SocketHandle{1}, data, [](core::Result<size_t, NetworkError>) {});

    EXPECT_TRUE(pal.wasWriteCalled());
}

TEST(NetworkPALTest, CloseSocketSavesHandle) {
    MockNetworkPAL pal;
    SocketHandle handle{42};

    pal.closeSocket(handle);

    EXPECT_EQ(pal.getClosedSocket(), handle);
}

TEST(NetworkPALTest, SetSocketOptionSavesValue) {
    MockNetworkPAL pal;
    auto result = pal.setSocketOption(SocketHandle{1}, SocketOption::NoDelay, 1);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(pal.getLastOption(), SocketOption::NoDelay);
    EXPECT_EQ(pal.getLastOptionValue(), 1);
}

TEST(NetworkPALTest, EventLoopControlMethods) {
    MockNetworkPAL pal;

    EXPECT_FALSE(pal.isRunning());

    pal.runEventLoop();
    EXPECT_TRUE(pal.isRunning());

    pal.stopEventLoop();
    EXPECT_FALSE(pal.isRunning());
}

// =============================================================================
// Thread PAL Interface Tests
// =============================================================================

class MockThreadPAL : public IThreadPAL {
public:
    // Thread creation
    core::Result<ThreadHandle, ThreadError> createThread(
        ThreadFunction func,
        void* arg,
        const ThreadOptions& options
    ) override {
        threadCreated_ = true;
        return core::Result<ThreadHandle, ThreadError>::success(ThreadHandle{1});
    }

    core::Result<void, ThreadError> joinThread(ThreadHandle handle) override {
        threadJoined_ = true;
        return core::Result<void, ThreadError>::success();
    }

    core::Result<void, ThreadError> detachThread(ThreadHandle handle) override {
        threadDetached_ = true;
        return core::Result<void, ThreadError>::success();
    }

    // Thread pool
    core::Result<ThreadPoolHandle, ThreadError> createThreadPool(
        size_t minThreads,
        size_t maxThreads,
        const ThreadPoolOptions& options
    ) override {
        poolCreated_ = true;
        minThreads_ = minThreads;
        maxThreads_ = maxThreads;
        return core::Result<ThreadPoolHandle, ThreadError>::success(ThreadPoolHandle{1});
    }

    core::Result<void, ThreadError> submitWork(
        ThreadPoolHandle pool,
        WorkItem work
    ) override {
        workSubmitted_ = true;
        return core::Result<void, ThreadError>::success();
    }

    core::Result<void, ThreadError> destroyThreadPool(
        ThreadPoolHandle pool
    ) override {
        poolDestroyed_ = true;
        return core::Result<void, ThreadError>::success();
    }

    // Synchronization primitives
    core::Result<MutexHandle, ThreadError> createMutex() override {
        return core::Result<MutexHandle, ThreadError>::success(MutexHandle{1});
    }

    void destroyMutex(MutexHandle handle) override {
        mutexDestroyed_ = true;
    }

    void lockMutex(MutexHandle handle) override {
        mutexLocked_ = true;
    }

    bool tryLockMutex(MutexHandle handle) override {
        return true;
    }

    void unlockMutex(MutexHandle handle) override {
        mutexUnlocked_ = true;
    }

    core::Result<ConditionVariableHandle, ThreadError> createConditionVariable() override {
        return core::Result<ConditionVariableHandle, ThreadError>::success(ConditionVariableHandle{1});
    }

    void destroyConditionVariable(ConditionVariableHandle handle) override {
        cvDestroyed_ = true;
    }

    void waitConditionVariable(ConditionVariableHandle cv, MutexHandle mutex) override {
        cvWaited_ = true;
    }

    bool waitConditionVariableFor(
        ConditionVariableHandle cv,
        MutexHandle mutex,
        std::chrono::milliseconds timeout
    ) override {
        cvWaitedTimed_ = true;
        return true;
    }

    void notifyOne(ConditionVariableHandle cv) override {
        cvNotifiedOne_ = true;
    }

    void notifyAll(ConditionVariableHandle cv) override {
        cvNotifiedAll_ = true;
    }

    // Current thread
    ThreadId getCurrentThreadId() override {
        return ThreadId{12345};
    }

    void setThreadName(const std::string& name) override {
        threadName_ = name;
    }

    void sleepFor(std::chrono::milliseconds duration) override {
        sleptFor_ = duration;
    }

    // Test accessors
    bool wasThreadCreated() const { return threadCreated_; }
    bool wasThreadJoined() const { return threadJoined_; }
    bool wasThreadDetached() const { return threadDetached_; }
    bool wasPoolCreated() const { return poolCreated_; }
    bool wasWorkSubmitted() const { return workSubmitted_; }
    bool wasPoolDestroyed() const { return poolDestroyed_; }
    bool wasMutexLocked() const { return mutexLocked_; }
    bool wasMutexUnlocked() const { return mutexUnlocked_; }
    bool wasCvWaited() const { return cvWaited_; }
    bool wasCvNotifiedOne() const { return cvNotifiedOne_; }
    bool wasCvNotifiedAll() const { return cvNotifiedAll_; }
    const std::string& getThreadName() const { return threadName_; }
    size_t getMinThreads() const { return minThreads_; }
    size_t getMaxThreads() const { return maxThreads_; }

private:
    bool threadCreated_ = false;
    bool threadJoined_ = false;
    bool threadDetached_ = false;
    bool poolCreated_ = false;
    bool workSubmitted_ = false;
    bool poolDestroyed_ = false;
    bool mutexDestroyed_ = false;
    bool mutexLocked_ = false;
    bool mutexUnlocked_ = false;
    bool cvDestroyed_ = false;
    bool cvWaited_ = false;
    bool cvWaitedTimed_ = false;
    bool cvNotifiedOne_ = false;
    bool cvNotifiedAll_ = false;
    std::string threadName_;
    size_t minThreads_ = 0;
    size_t maxThreads_ = 0;
    std::chrono::milliseconds sleptFor_{0};
};

TEST(ThreadPALTest, InterfaceCompiles) {
    MockThreadPAL pal;
    IThreadPAL* interface = &pal;
    EXPECT_NE(interface, nullptr);
}

TEST(ThreadPALTest, CreateThreadReturnsValidHandle) {
    MockThreadPAL pal;
    auto result = pal.createThread([](void*) {}, nullptr, ThreadOptions{});

    EXPECT_TRUE(result.isSuccess());
    EXPECT_NE(result.value(), INVALID_THREAD_HANDLE);
    EXPECT_TRUE(pal.wasThreadCreated());
}

TEST(ThreadPALTest, JoinThreadWorks) {
    MockThreadPAL pal;
    auto result = pal.joinThread(ThreadHandle{1});

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(pal.wasThreadJoined());
}

TEST(ThreadPALTest, DetachThreadWorks) {
    MockThreadPAL pal;
    auto result = pal.detachThread(ThreadHandle{1});

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(pal.wasThreadDetached());
}

TEST(ThreadPALTest, CreateThreadPoolReturnsValidHandle) {
    MockThreadPAL pal;
    auto result = pal.createThreadPool(2, 8, ThreadPoolOptions{});

    EXPECT_TRUE(result.isSuccess());
    EXPECT_NE(result.value(), INVALID_THREAD_POOL_HANDLE);
    EXPECT_TRUE(pal.wasPoolCreated());
    EXPECT_EQ(pal.getMinThreads(), 2u);
    EXPECT_EQ(pal.getMaxThreads(), 8u);
}

TEST(ThreadPALTest, SubmitWorkWorks) {
    MockThreadPAL pal;
    auto result = pal.submitWork(ThreadPoolHandle{1}, []() {});

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(pal.wasWorkSubmitted());
}

TEST(ThreadPALTest, MutexOperationsWork) {
    MockThreadPAL pal;

    auto createResult = pal.createMutex();
    EXPECT_TRUE(createResult.isSuccess());

    MutexHandle mutex = createResult.value();
    pal.lockMutex(mutex);
    EXPECT_TRUE(pal.wasMutexLocked());

    pal.unlockMutex(mutex);
    EXPECT_TRUE(pal.wasMutexUnlocked());
}

TEST(ThreadPALTest, ConditionVariableOperationsWork) {
    MockThreadPAL pal;

    auto cvResult = pal.createConditionVariable();
    EXPECT_TRUE(cvResult.isSuccess());

    auto mutexResult = pal.createMutex();
    EXPECT_TRUE(mutexResult.isSuccess());

    ConditionVariableHandle cv = cvResult.value();
    MutexHandle mutex = mutexResult.value();

    pal.notifyOne(cv);
    EXPECT_TRUE(pal.wasCvNotifiedOne());

    pal.notifyAll(cv);
    EXPECT_TRUE(pal.wasCvNotifiedAll());
}

TEST(ThreadPALTest, GetCurrentThreadIdReturnsNonZero) {
    MockThreadPAL pal;
    ThreadId id = pal.getCurrentThreadId();

    EXPECT_NE(id, ThreadId{0});
}

TEST(ThreadPALTest, SetThreadNameWorks) {
    MockThreadPAL pal;
    pal.setThreadName("WorkerThread");

    EXPECT_EQ(pal.getThreadName(), "WorkerThread");
}

// =============================================================================
// Timer PAL Interface Tests
// =============================================================================

class MockTimerPAL : public ITimerPAL {
public:
    core::Result<TimerHandle, TimerError> scheduleOnce(
        std::chrono::milliseconds delay,
        TimerCallback callback
    ) override {
        onceScheduled_ = true;
        lastDelay_ = delay;
        return core::Result<TimerHandle, TimerError>::success(TimerHandle{1});
    }

    core::Result<TimerHandle, TimerError> scheduleRepeating(
        std::chrono::milliseconds interval,
        TimerCallback callback
    ) override {
        repeatingScheduled_ = true;
        lastInterval_ = interval;
        return core::Result<TimerHandle, TimerError>::success(TimerHandle{2});
    }

    core::Result<void, TimerError> cancelTimer(TimerHandle handle) override {
        cancelledHandle_ = handle;
        return core::Result<void, TimerError>::success();
    }

    std::chrono::steady_clock::time_point now() const override {
        return std::chrono::steady_clock::now();
    }

    uint64_t getMonotonicMillis() const override {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
    }

    // Test accessors
    bool wasOnceScheduled() const { return onceScheduled_; }
    bool wasRepeatingScheduled() const { return repeatingScheduled_; }
    std::chrono::milliseconds getLastDelay() const { return lastDelay_; }
    std::chrono::milliseconds getLastInterval() const { return lastInterval_; }
    TimerHandle getCancelledHandle() const { return cancelledHandle_; }

private:
    bool onceScheduled_ = false;
    bool repeatingScheduled_ = false;
    std::chrono::milliseconds lastDelay_{0};
    std::chrono::milliseconds lastInterval_{0};
    TimerHandle cancelledHandle_ = INVALID_TIMER_HANDLE;
};

TEST(TimerPALTest, InterfaceCompiles) {
    MockTimerPAL pal;
    ITimerPAL* interface = &pal;
    EXPECT_NE(interface, nullptr);
}

TEST(TimerPALTest, ScheduleOnceReturnsValidHandle) {
    MockTimerPAL pal;
    auto result = pal.scheduleOnce(std::chrono::milliseconds{100}, []() {});

    EXPECT_TRUE(result.isSuccess());
    EXPECT_NE(result.value(), INVALID_TIMER_HANDLE);
    EXPECT_TRUE(pal.wasOnceScheduled());
    EXPECT_EQ(pal.getLastDelay(), std::chrono::milliseconds{100});
}

TEST(TimerPALTest, ScheduleRepeatingReturnsValidHandle) {
    MockTimerPAL pal;
    auto result = pal.scheduleRepeating(std::chrono::milliseconds{500}, []() {});

    EXPECT_TRUE(result.isSuccess());
    EXPECT_NE(result.value(), INVALID_TIMER_HANDLE);
    EXPECT_TRUE(pal.wasRepeatingScheduled());
    EXPECT_EQ(pal.getLastInterval(), std::chrono::milliseconds{500});
}

TEST(TimerPALTest, CancelTimerWorks) {
    MockTimerPAL pal;
    TimerHandle handle{42};
    auto result = pal.cancelTimer(handle);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(pal.getCancelledHandle(), handle);
}

TEST(TimerPALTest, NowReturnsValidTimePoint) {
    MockTimerPAL pal;
    auto before = std::chrono::steady_clock::now();
    auto now = pal.now();
    auto after = std::chrono::steady_clock::now();

    EXPECT_GE(now, before);
    EXPECT_LE(now, after);
}

TEST(TimerPALTest, GetMonotonicMillisReturnsNonZero) {
    MockTimerPAL pal;
    uint64_t millis = pal.getMonotonicMillis();

    EXPECT_GT(millis, 0u);
}

// =============================================================================
// File PAL Interface Tests
// =============================================================================

class MockFilePAL : public IFilePAL {
public:
    core::Result<FileHandle, FileError> open(
        const std::string& path,
        FileOpenMode mode
    ) override {
        lastPath_ = path;
        lastMode_ = mode;
        return core::Result<FileHandle, FileError>::success(FileHandle{1});
    }

    core::Result<void, FileError> close(FileHandle handle) override {
        closedHandle_ = handle;
        return core::Result<void, FileError>::success();
    }

    core::Result<size_t, FileError> read(
        FileHandle handle,
        core::Buffer& buffer,
        size_t maxBytes
    ) override {
        readCalled_ = true;
        return core::Result<size_t, FileError>::success(maxBytes);
    }

    core::Result<size_t, FileError> write(
        FileHandle handle,
        const core::Buffer& data
    ) override {
        writeCalled_ = true;
        return core::Result<size_t, FileError>::success(data.size());
    }

    core::Result<size_t, FileError> seek(
        FileHandle handle,
        int64_t offset,
        SeekOrigin origin
    ) override {
        seekCalled_ = true;
        return core::Result<size_t, FileError>::success(static_cast<size_t>(offset));
    }

    core::Result<size_t, FileError> tell(FileHandle handle) const override {
        return core::Result<size_t, FileError>::success(0);
    }

    core::Result<size_t, FileError> size(FileHandle handle) const override {
        return core::Result<size_t, FileError>::success(1024);
    }

    core::Result<void, FileError> flush(FileHandle handle) override {
        flushCalled_ = true;
        return core::Result<void, FileError>::success();
    }

    core::Result<bool, FileError> exists(const std::string& path) const override {
        return core::Result<bool, FileError>::success(true);
    }

    core::Result<void, FileError> remove(const std::string& path) override {
        removedPath_ = path;
        return core::Result<void, FileError>::success();
    }

    core::Result<void, FileError> rename(
        const std::string& oldPath,
        const std::string& newPath
    ) override {
        renamedFrom_ = oldPath;
        renamedTo_ = newPath;
        return core::Result<void, FileError>::success();
    }

    core::Result<void, FileError> createDirectory(const std::string& path) override {
        createdDir_ = path;
        return core::Result<void, FileError>::success();
    }

    // Test accessors
    const std::string& getLastPath() const { return lastPath_; }
    FileOpenMode getLastMode() const { return lastMode_; }
    FileHandle getClosedHandle() const { return closedHandle_; }
    bool wasReadCalled() const { return readCalled_; }
    bool wasWriteCalled() const { return writeCalled_; }
    bool wasSeekCalled() const { return seekCalled_; }
    bool wasFlushCalled() const { return flushCalled_; }
    const std::string& getRemovedPath() const { return removedPath_; }
    const std::string& getRenamedFrom() const { return renamedFrom_; }
    const std::string& getRenamedTo() const { return renamedTo_; }
    const std::string& getCreatedDir() const { return createdDir_; }

private:
    std::string lastPath_;
    FileOpenMode lastMode_ = FileOpenMode::Read;
    FileHandle closedHandle_ = INVALID_FILE_HANDLE;
    bool readCalled_ = false;
    bool writeCalled_ = false;
    bool seekCalled_ = false;
    bool flushCalled_ = false;
    std::string removedPath_;
    std::string renamedFrom_;
    std::string renamedTo_;
    std::string createdDir_;
};

TEST(FilePALTest, InterfaceCompiles) {
    MockFilePAL pal;
    IFilePAL* interface = &pal;
    EXPECT_NE(interface, nullptr);
}

TEST(FilePALTest, OpenReturnsValidHandle) {
    MockFilePAL pal;
    auto result = pal.open("/path/to/file.txt", FileOpenMode::Read);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_NE(result.value(), INVALID_FILE_HANDLE);
    EXPECT_EQ(pal.getLastPath(), "/path/to/file.txt");
    EXPECT_EQ(pal.getLastMode(), FileOpenMode::Read);
}

TEST(FilePALTest, CloseWorks) {
    MockFilePAL pal;
    FileHandle handle{42};
    auto result = pal.close(handle);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(pal.getClosedHandle(), handle);
}

TEST(FilePALTest, ReadWorks) {
    MockFilePAL pal;
    core::Buffer buffer;
    auto result = pal.read(FileHandle{1}, buffer, 1024);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(pal.wasReadCalled());
}

TEST(FilePALTest, WriteWorks) {
    MockFilePAL pal;
    core::Buffer data{0x01, 0x02, 0x03};
    auto result = pal.write(FileHandle{1}, data);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(pal.wasWriteCalled());
}

TEST(FilePALTest, SeekWorks) {
    MockFilePAL pal;
    auto result = pal.seek(FileHandle{1}, 100, SeekOrigin::Begin);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(pal.wasSeekCalled());
}

TEST(FilePALTest, FlushWorks) {
    MockFilePAL pal;
    auto result = pal.flush(FileHandle{1});

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(pal.wasFlushCalled());
}

TEST(FilePALTest, ExistsWorks) {
    MockFilePAL pal;
    auto result = pal.exists("/path/to/file.txt");

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(result.value());
}

TEST(FilePALTest, RemoveWorks) {
    MockFilePAL pal;
    auto result = pal.remove("/path/to/delete.txt");

    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(pal.getRemovedPath(), "/path/to/delete.txt");
}

TEST(FilePALTest, RenameWorks) {
    MockFilePAL pal;
    auto result = pal.rename("/old/path.txt", "/new/path.txt");

    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(pal.getRenamedFrom(), "/old/path.txt");
    EXPECT_EQ(pal.getRenamedTo(), "/new/path.txt");
}

TEST(FilePALTest, CreateDirectoryWorks) {
    MockFilePAL pal;
    auto result = pal.createDirectory("/path/to/newdir");

    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(pal.getCreatedDir(), "/path/to/newdir");
}

// =============================================================================
// Log PAL Interface Tests
// =============================================================================

class MockLogPAL : public ILogPAL {
public:
    void log(
        LogLevel level,
        const std::string& message,
        const std::string& category,
        const LogContext& context
    ) override {
        lastLevel_ = level;
        lastMessage_ = message;
        lastCategory_ = category;
        logCalled_ = true;
    }

    void setMinLevel(LogLevel level) override {
        minLevel_ = level;
    }

    LogLevel getMinLevel() const override {
        return minLevel_;
    }

    void flush() override {
        flushCalled_ = true;
    }

    void addSink(std::shared_ptr<ILogSink> sink) override {
        sinkAdded_ = true;
    }

    void removeSink(std::shared_ptr<ILogSink> sink) override {
        sinkRemoved_ = true;
    }

    // Test accessors
    bool wasLogCalled() const { return logCalled_; }
    LogLevel getLastLevel() const { return lastLevel_; }
    const std::string& getLastMessage() const { return lastMessage_; }
    const std::string& getLastCategory() const { return lastCategory_; }
    bool wasFlushCalled() const { return flushCalled_; }
    bool wasSinkAdded() const { return sinkAdded_; }
    bool wasSinkRemoved() const { return sinkRemoved_; }

private:
    bool logCalled_ = false;
    LogLevel lastLevel_ = LogLevel::Info;
    std::string lastMessage_;
    std::string lastCategory_;
    LogLevel minLevel_ = LogLevel::Debug;
    bool flushCalled_ = false;
    bool sinkAdded_ = false;
    bool sinkRemoved_ = false;
};

TEST(LogPALTest, InterfaceCompiles) {
    MockLogPAL pal;
    ILogPAL* interface = &pal;
    EXPECT_NE(interface, nullptr);
}

TEST(LogPALTest, LogWorks) {
    MockLogPAL pal;
    LogContext ctx;
    ctx.file = "test.cpp";
    ctx.line = 42;

    pal.log(LogLevel::Info, "Test message", "Test", ctx);

    EXPECT_TRUE(pal.wasLogCalled());
    EXPECT_EQ(pal.getLastLevel(), LogLevel::Info);
    EXPECT_EQ(pal.getLastMessage(), "Test message");
    EXPECT_EQ(pal.getLastCategory(), "Test");
}

TEST(LogPALTest, SetMinLevelWorks) {
    MockLogPAL pal;

    pal.setMinLevel(LogLevel::Warning);
    EXPECT_EQ(pal.getMinLevel(), LogLevel::Warning);

    pal.setMinLevel(LogLevel::Error);
    EXPECT_EQ(pal.getMinLevel(), LogLevel::Error);
}

TEST(LogPALTest, FlushWorks) {
    MockLogPAL pal;
    pal.flush();

    EXPECT_TRUE(pal.wasFlushCalled());
}

TEST(LogPALTest, AddSinkWorks) {
    MockLogPAL pal;
    pal.addSink(nullptr);

    EXPECT_TRUE(pal.wasSinkAdded());
}

TEST(LogPALTest, RemoveSinkWorks) {
    MockLogPAL pal;
    pal.removeSink(nullptr);

    EXPECT_TRUE(pal.wasSinkRemoved());
}

// =============================================================================
// Callback Type Tests
// =============================================================================

TEST(CallbackTypesTest, AcceptCallbackTypeValid) {
    AcceptCallback callback = [](core::Result<SocketHandle, NetworkError> result) {
        EXPECT_TRUE(result.isSuccess());
    };

    callback(core::Result<SocketHandle, NetworkError>::success(SocketHandle{1}));
}

TEST(CallbackTypesTest, ReadCallbackTypeValid) {
    ReadCallback callback = [](core::Result<size_t, NetworkError> result) {
        EXPECT_TRUE(result.isSuccess());
        EXPECT_EQ(result.value(), 100u);
    };

    callback(core::Result<size_t, NetworkError>::success(100));
}

TEST(CallbackTypesTest, WriteCallbackTypeValid) {
    WriteCallback callback = [](core::Result<size_t, NetworkError> result) {
        EXPECT_TRUE(result.isSuccess());
        EXPECT_EQ(result.value(), 50u);
    };

    callback(core::Result<size_t, NetworkError>::success(50));
}

TEST(CallbackTypesTest, TimerCallbackTypeValid) {
    bool called = false;
    TimerCallback callback = [&called]() {
        called = true;
    };

    callback();
    EXPECT_TRUE(called);
}

TEST(CallbackTypesTest, ThreadFunctionTypeValid) {
    int value = 0;
    ThreadFunction func = [](void* arg) {
        int* ptr = static_cast<int*>(arg);
        *ptr = 42;
    };

    func(&value);
    EXPECT_EQ(value, 42);
}

TEST(CallbackTypesTest, WorkItemTypeValid) {
    int value = 0;
    WorkItem work = [&value]() {
        value = 100;
    };

    work();
    EXPECT_EQ(value, 100);
}

} // namespace test
} // namespace pal
} // namespace openrtmp
