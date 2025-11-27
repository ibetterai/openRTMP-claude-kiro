// OpenRTMP - Cross-platform RTMP Server
// Tests for Linux/Android Thread PAL Implementation
//
// Requirements Covered: 6.3 (Threading abstraction)

#include <gtest/gtest.h>
#include "openrtmp/pal/thread_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"

#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

#if defined(__linux__) || defined(__ANDROID__)
#include "openrtmp/pal/linux/linux_thread_pal.hpp"
#include <pthread.h>
#endif

namespace openrtmp {
namespace pal {
namespace test {

#if defined(__linux__) || defined(__ANDROID__)

// =============================================================================
// Linux Thread PAL Tests
// =============================================================================

class LinuxThreadPALTest : public ::testing::Test {
protected:
    void SetUp() override {
        threadPal_ = std::make_unique<linux::LinuxThreadPAL>();
    }

    void TearDown() override {
        threadPal_.reset();
    }

    std::unique_ptr<linux::LinuxThreadPAL> threadPal_;
};

TEST_F(LinuxThreadPALTest, ImplementsIThreadPALInterface) {
    IThreadPAL* interface = threadPal_.get();
    EXPECT_NE(interface, nullptr);
}

// =============================================================================
// Thread Creation Tests
// =============================================================================

TEST_F(LinuxThreadPALTest, CreateThreadReturnsValidHandle) {
    std::atomic<bool> executed{false};

    auto result = threadPal_->createThread(
        [](void* arg) {
            auto* flag = static_cast<std::atomic<bool>*>(arg);
            *flag = true;
        },
        &executed,
        ThreadOptions{}
    );

    EXPECT_TRUE(result.isSuccess());
    EXPECT_NE(result.value(), INVALID_THREAD_HANDLE);

    // Join to ensure thread completes
    auto joinResult = threadPal_->joinThread(result.value());
    EXPECT_TRUE(joinResult.isSuccess());
    EXPECT_TRUE(executed.load());
}

TEST_F(LinuxThreadPALTest, ThreadExecutesFunction) {
    std::atomic<int> value{0};

    auto result = threadPal_->createThread(
        [](void* arg) {
            auto* val = static_cast<std::atomic<int>*>(arg);
            *val = 42;
        },
        &value,
        ThreadOptions{}
    );

    ASSERT_TRUE(result.isSuccess());

    auto joinResult = threadPal_->joinThread(result.value());
    EXPECT_TRUE(joinResult.isSuccess());
    EXPECT_EQ(value.load(), 42);
}

TEST_F(LinuxThreadPALTest, CreateThreadWithName) {
    ThreadOptions opts;
    opts.name = "TestWorker";

    auto result = threadPal_->createThread(
        [](void*) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        },
        nullptr,
        opts
    );

    EXPECT_TRUE(result.isSuccess());

    auto joinResult = threadPal_->joinThread(result.value());
    EXPECT_TRUE(joinResult.isSuccess());
}

TEST_F(LinuxThreadPALTest, JoinThreadWaitsForCompletion) {
    std::atomic<bool> completed{false};

    auto result = threadPal_->createThread(
        [](void* arg) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto* flag = static_cast<std::atomic<bool>*>(arg);
            *flag = true;
        },
        &completed,
        ThreadOptions{}
    );

    ASSERT_TRUE(result.isSuccess());
    EXPECT_FALSE(completed.load());

    auto joinResult = threadPal_->joinThread(result.value());
    EXPECT_TRUE(joinResult.isSuccess());
    EXPECT_TRUE(completed.load());
}

TEST_F(LinuxThreadPALTest, DetachThreadAllowsIndependentExecution) {
    std::atomic<bool> completed{false};

    auto result = threadPal_->createThread(
        [](void* arg) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto* flag = static_cast<std::atomic<bool>*>(arg);
            *flag = true;
        },
        &completed,
        ThreadOptions{}
    );

    ASSERT_TRUE(result.isSuccess());

    auto detachResult = threadPal_->detachThread(result.value());
    EXPECT_TRUE(detachResult.isSuccess());

    // Wait for detached thread to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(completed.load());
}

TEST_F(LinuxThreadPALTest, CreateMultipleThreads) {
    std::atomic<int> counter{0};
    std::vector<ThreadHandle> handles;

    for (int i = 0; i < 10; ++i) {
        auto result = threadPal_->createThread(
            [](void* arg) {
                auto* cnt = static_cast<std::atomic<int>*>(arg);
                (*cnt)++;
            },
            &counter,
            ThreadOptions{}
        );

        ASSERT_TRUE(result.isSuccess());
        handles.push_back(result.value());
    }

    // Join all threads
    for (auto& handle : handles) {
        auto joinResult = threadPal_->joinThread(handle);
        EXPECT_TRUE(joinResult.isSuccess());
    }

    EXPECT_EQ(counter.load(), 10);
}

// =============================================================================
// Thread Pool Tests
// =============================================================================

TEST_F(LinuxThreadPALTest, CreateThreadPoolReturnsValidHandle) {
    ThreadPoolOptions opts;
    opts.name = "TestPool";

    auto result = threadPal_->createThreadPool(2, 4, opts);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_NE(result.value(), INVALID_THREAD_POOL_HANDLE);

    auto destroyResult = threadPal_->destroyThreadPool(result.value());
    EXPECT_TRUE(destroyResult.isSuccess());
}

TEST_F(LinuxThreadPALTest, SubmitWorkExecutes) {
    ThreadPoolOptions opts;
    opts.name = "WorkPool";

    auto poolResult = threadPal_->createThreadPool(2, 4, opts);
    ASSERT_TRUE(poolResult.isSuccess());

    std::atomic<bool> executed{false};

    auto submitResult = threadPal_->submitWork(
        poolResult.value(),
        [&executed]() { executed = true; }
    );

    EXPECT_TRUE(submitResult.isSuccess());

    // Wait for work to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(executed.load());

    auto destroyResult = threadPal_->destroyThreadPool(poolResult.value());
    EXPECT_TRUE(destroyResult.isSuccess());
}

TEST_F(LinuxThreadPALTest, SubmitMultipleWorkItems) {
    ThreadPoolOptions opts;
    opts.name = "MultiWorkPool";

    auto poolResult = threadPal_->createThreadPool(2, 8, opts);
    ASSERT_TRUE(poolResult.isSuccess());

    std::atomic<int> counter{0};

    for (int i = 0; i < 100; ++i) {
        auto submitResult = threadPal_->submitWork(
            poolResult.value(),
            [&counter]() { counter++; }
        );
        EXPECT_TRUE(submitResult.isSuccess());
    }

    // Wait for all work to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(counter.load(), 100);

    auto destroyResult = threadPal_->destroyThreadPool(poolResult.value());
    EXPECT_TRUE(destroyResult.isSuccess());
}

TEST_F(LinuxThreadPALTest, DestroyPoolWaitsForActiveWork) {
    ThreadPoolOptions opts;
    auto poolResult = threadPal_->createThreadPool(1, 2, opts);
    ASSERT_TRUE(poolResult.isSuccess());

    std::atomic<bool> workStarted{false};
    std::atomic<bool> workCompleted{false};

    auto submitResult = threadPal_->submitWork(
        poolResult.value(),
        [&workStarted, &workCompleted]() {
            workStarted = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            workCompleted = true;
        }
    );
    EXPECT_TRUE(submitResult.isSuccess());

    // Wait for work to start
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(workStarted.load());

    // Destroy should wait for work to complete
    auto destroyResult = threadPal_->destroyThreadPool(poolResult.value());
    EXPECT_TRUE(destroyResult.isSuccess());
    EXPECT_TRUE(workCompleted.load());
}

// =============================================================================
// Mutex Tests
// =============================================================================

TEST_F(LinuxThreadPALTest, CreateMutexReturnsValidHandle) {
    auto result = threadPal_->createMutex();

    EXPECT_TRUE(result.isSuccess());
    EXPECT_NE(result.value(), INVALID_MUTEX_HANDLE);

    threadPal_->destroyMutex(result.value());
}

TEST_F(LinuxThreadPALTest, MutexLockUnlock) {
    auto mutexResult = threadPal_->createMutex();
    ASSERT_TRUE(mutexResult.isSuccess());

    MutexHandle mutex = mutexResult.value();

    EXPECT_NO_THROW(threadPal_->lockMutex(mutex));
    EXPECT_NO_THROW(threadPal_->unlockMutex(mutex));

    threadPal_->destroyMutex(mutex);
}

TEST_F(LinuxThreadPALTest, TryLockMutexWhenUnlocked) {
    auto mutexResult = threadPal_->createMutex();
    ASSERT_TRUE(mutexResult.isSuccess());

    MutexHandle mutex = mutexResult.value();

    EXPECT_TRUE(threadPal_->tryLockMutex(mutex));
    threadPal_->unlockMutex(mutex);

    threadPal_->destroyMutex(mutex);
}

TEST_F(LinuxThreadPALTest, TryLockMutexWhenLocked) {
    auto mutexResult = threadPal_->createMutex();
    ASSERT_TRUE(mutexResult.isSuccess());

    MutexHandle mutex = mutexResult.value();

    // Lock the mutex
    threadPal_->lockMutex(mutex);

    // Create a thread that will try to lock
    std::atomic<bool> tryLockSucceeded{true};
    std::atomic<bool> threadStarted{false};

    std::thread t([this, mutex, &tryLockSucceeded, &threadStarted]() {
        threadStarted = true;
        // Try to lock - should fail since we hold the lock
        tryLockSucceeded = threadPal_->tryLockMutex(mutex);
    });

    // Wait for thread to start and attempt lock
    while (!threadStarted) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    t.join();

    // Try to lock should have failed
    EXPECT_FALSE(tryLockSucceeded.load());

    threadPal_->unlockMutex(mutex);
    threadPal_->destroyMutex(mutex);
}

TEST_F(LinuxThreadPALTest, MutexProtectsSharedData) {
    auto mutexResult = threadPal_->createMutex();
    ASSERT_TRUE(mutexResult.isSuccess());

    MutexHandle mutex = mutexResult.value();
    int sharedCounter = 0;

    auto incrementer = [this, mutex, &sharedCounter](void*) {
        for (int i = 0; i < 1000; ++i) {
            threadPal_->lockMutex(mutex);
            sharedCounter++;
            threadPal_->unlockMutex(mutex);
        }
    };

    // Create multiple threads
    std::vector<ThreadHandle> handles;
    for (int i = 0; i < 4; ++i) {
        auto result = threadPal_->createThread(incrementer, nullptr, ThreadOptions{});
        ASSERT_TRUE(result.isSuccess());
        handles.push_back(result.value());
    }

    // Join all threads
    for (auto& handle : handles) {
        threadPal_->joinThread(handle);
    }

    EXPECT_EQ(sharedCounter, 4000);

    threadPal_->destroyMutex(mutex);
}

// =============================================================================
// Condition Variable Tests
// =============================================================================

TEST_F(LinuxThreadPALTest, CreateConditionVariableReturnsValidHandle) {
    auto result = threadPal_->createConditionVariable();

    EXPECT_TRUE(result.isSuccess());
    EXPECT_NE(result.value(), INVALID_CONDITION_VARIABLE_HANDLE);

    threadPal_->destroyConditionVariable(result.value());
}

TEST_F(LinuxThreadPALTest, ConditionVariableNotifyOneWakesOneThread) {
    auto cvResult = threadPal_->createConditionVariable();
    auto mutexResult = threadPal_->createMutex();
    ASSERT_TRUE(cvResult.isSuccess());
    ASSERT_TRUE(mutexResult.isSuccess());

    ConditionVariableHandle cv = cvResult.value();
    MutexHandle mutex = mutexResult.value();
    std::atomic<bool> ready{false};
    std::atomic<bool> woken{false};

    struct ThreadData {
        linux::LinuxThreadPAL* pal;
        ConditionVariableHandle cv;
        MutexHandle mutex;
        std::atomic<bool>* ready;
        std::atomic<bool>* woken;
    };

    ThreadData data{threadPal_.get(), cv, mutex, &ready, &woken};

    auto threadResult = threadPal_->createThread(
        [](void* arg) {
            auto* td = static_cast<ThreadData*>(arg);
            td->pal->lockMutex(td->mutex);
            *(td->ready) = true;
            while (!td->woken->load()) {
                td->pal->waitConditionVariable(td->cv, td->mutex);
            }
            td->pal->unlockMutex(td->mutex);
        },
        &data,
        ThreadOptions{}
    );

    ASSERT_TRUE(threadResult.isSuccess());

    // Wait for thread to start waiting
    while (!ready.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Signal the condition variable
    threadPal_->lockMutex(mutex);
    woken = true;
    threadPal_->notifyOne(cv);
    threadPal_->unlockMutex(mutex);

    threadPal_->joinThread(threadResult.value());

    EXPECT_TRUE(woken.load());

    threadPal_->destroyConditionVariable(cv);
    threadPal_->destroyMutex(mutex);
}

TEST_F(LinuxThreadPALTest, WaitConditionVariableForReturnsOnTimeout) {
    auto cvResult = threadPal_->createConditionVariable();
    auto mutexResult = threadPal_->createMutex();
    ASSERT_TRUE(cvResult.isSuccess());
    ASSERT_TRUE(mutexResult.isSuccess());

    ConditionVariableHandle cv = cvResult.value();
    MutexHandle mutex = mutexResult.value();

    threadPal_->lockMutex(mutex);

    auto start = std::chrono::steady_clock::now();
    bool signaled = threadPal_->waitConditionVariableFor(cv, mutex, std::chrono::milliseconds{50});
    auto end = std::chrono::steady_clock::now();

    threadPal_->unlockMutex(mutex);

    EXPECT_FALSE(signaled);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    EXPECT_GE(elapsed.count(), 40);  // Should wait at least close to timeout

    threadPal_->destroyConditionVariable(cv);
    threadPal_->destroyMutex(mutex);
}

TEST_F(LinuxThreadPALTest, NotifyAllWakesAllThreads) {
    auto cvResult = threadPal_->createConditionVariable();
    auto mutexResult = threadPal_->createMutex();
    ASSERT_TRUE(cvResult.isSuccess());
    ASSERT_TRUE(mutexResult.isSuccess());

    ConditionVariableHandle cv = cvResult.value();
    MutexHandle mutex = mutexResult.value();
    std::atomic<int> readyCount{0};
    std::atomic<bool> go{false};
    std::atomic<int> doneCount{0};

    struct ThreadData {
        linux::LinuxThreadPAL* pal;
        ConditionVariableHandle cv;
        MutexHandle mutex;
        std::atomic<int>* readyCount;
        std::atomic<bool>* go;
        std::atomic<int>* doneCount;
    };

    ThreadData data{threadPal_.get(), cv, mutex, &readyCount, &go, &doneCount};

    std::vector<ThreadHandle> handles;
    for (int i = 0; i < 5; ++i) {
        auto result = threadPal_->createThread(
            [](void* arg) {
                auto* td = static_cast<ThreadData*>(arg);
                td->pal->lockMutex(td->mutex);
                (*(td->readyCount))++;
                while (!td->go->load()) {
                    td->pal->waitConditionVariable(td->cv, td->mutex);
                }
                (*(td->doneCount))++;
                td->pal->unlockMutex(td->mutex);
            },
            &data,
            ThreadOptions{}
        );
        ASSERT_TRUE(result.isSuccess());
        handles.push_back(result.value());
    }

    // Wait for all threads to start waiting
    while (readyCount.load() < 5) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Signal all threads
    threadPal_->lockMutex(mutex);
    go = true;
    threadPal_->notifyAll(cv);
    threadPal_->unlockMutex(mutex);

    // Join all threads
    for (auto& handle : handles) {
        threadPal_->joinThread(handle);
    }

    EXPECT_EQ(doneCount.load(), 5);

    threadPal_->destroyConditionVariable(cv);
    threadPal_->destroyMutex(mutex);
}

// =============================================================================
// Current Thread Operations Tests
// =============================================================================

TEST_F(LinuxThreadPALTest, GetCurrentThreadIdReturnsNonZero) {
    ThreadId id = threadPal_->getCurrentThreadId();
    EXPECT_NE(id, ThreadId{0});
}

TEST_F(LinuxThreadPALTest, DifferentThreadsHaveDifferentIds) {
    ThreadId mainThreadId = threadPal_->getCurrentThreadId();
    std::atomic<uint64_t> otherThreadId{0};

    auto result = threadPal_->createThread(
        [](void* arg) {
            // Can't easily access threadPal_ from here, so we'll use pthread directly
            auto* id = static_cast<std::atomic<uint64_t>*>(arg);
            *id = static_cast<uint64_t>(pthread_self());
        },
        &otherThreadId,
        ThreadOptions{}
    );

    ASSERT_TRUE(result.isSuccess());
    threadPal_->joinThread(result.value());

    EXPECT_NE(mainThreadId.value, otherThreadId.load());
}

TEST_F(LinuxThreadPALTest, SetThreadNameDoesNotCrash) {
    EXPECT_NO_THROW(threadPal_->setThreadName("TestThread"));
}

TEST_F(LinuxThreadPALTest, SleepForSleepsAtLeastSpecifiedDuration) {
    auto start = std::chrono::steady_clock::now();
    threadPal_->sleepFor(std::chrono::milliseconds{50});
    auto end = std::chrono::steady_clock::now();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    EXPECT_GE(elapsed.count(), 45);  // Allow small tolerance
}

#endif // defined(__linux__) || defined(__ANDROID__)

} // namespace test
} // namespace pal
} // namespace openrtmp
