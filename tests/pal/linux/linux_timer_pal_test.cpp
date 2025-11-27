// OpenRTMP - Cross-platform RTMP Server
// Tests for Linux/Android Timer PAL Implementation
//
// Requirements Covered: 6.5 (Timer abstraction)

#include <gtest/gtest.h>
#include "openrtmp/pal/timer_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"

#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>

#if defined(__linux__) || defined(__ANDROID__)
#include "openrtmp/pal/linux/linux_timer_pal.hpp"
#endif

namespace openrtmp {
namespace pal {
namespace test {

#if defined(__linux__) || defined(__ANDROID__)

// =============================================================================
// Linux Timer PAL Tests
// =============================================================================

class LinuxTimerPALTest : public ::testing::Test {
protected:
    void SetUp() override {
        timerPal_ = std::make_unique<linux::LinuxTimerPAL>();
    }

    void TearDown() override {
        timerPal_.reset();
    }

    std::unique_ptr<linux::LinuxTimerPAL> timerPal_;
};

TEST_F(LinuxTimerPALTest, ImplementsITimerPALInterface) {
    ITimerPAL* interface = timerPal_.get();
    EXPECT_NE(interface, nullptr);
}

// =============================================================================
// One-Shot Timer Tests
// =============================================================================

TEST_F(LinuxTimerPALTest, ScheduleOnceReturnsValidHandle) {
    std::atomic<bool> fired{false};

    auto result = timerPal_->scheduleOnce(
        std::chrono::milliseconds{50},
        [&fired]() { fired = true; }
    );

    EXPECT_TRUE(result.isSuccess());
    EXPECT_NE(result.value(), INVALID_TIMER_HANDLE);

    // Wait for timer to fire
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_TRUE(fired.load());
}

TEST_F(LinuxTimerPALTest, ScheduleOnceFiresAfterDelay) {
    std::atomic<bool> fired{false};
    auto startTime = std::chrono::steady_clock::now();
    std::atomic<int64_t> fireTime{0};

    auto result = timerPal_->scheduleOnce(
        std::chrono::milliseconds{100},
        [&fired, &fireTime, startTime]() {
            auto now = std::chrono::steady_clock::now();
            fireTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - startTime).count();
            fired = true;
        }
    );

    ASSERT_TRUE(result.isSuccess());

    // Wait for timer to fire
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_TRUE(fired.load());
    // Timer should fire after ~100ms (allow some tolerance)
    EXPECT_GE(fireTime.load(), 90);
    EXPECT_LE(fireTime.load(), 200);
}

TEST_F(LinuxTimerPALTest, ScheduleOnceZeroDelayFiresImmediately) {
    std::atomic<bool> fired{false};

    auto result = timerPal_->scheduleOnce(
        std::chrono::milliseconds{0},
        [&fired]() { fired = true; }
    );

    ASSERT_TRUE(result.isSuccess());

    // Wait a short time for timer to fire
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(fired.load());
}

TEST_F(LinuxTimerPALTest, CancelOneShotTimerPreventsCallback) {
    std::atomic<bool> fired{false};

    auto result = timerPal_->scheduleOnce(
        std::chrono::milliseconds{100},
        [&fired]() { fired = true; }
    );

    ASSERT_TRUE(result.isSuccess());

    // Cancel immediately
    auto cancelResult = timerPal_->cancelTimer(result.value());
    EXPECT_TRUE(cancelResult.isSuccess());

    // Wait past the timer deadline
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(fired.load());
}

// =============================================================================
// Repeating Timer Tests
// =============================================================================

TEST_F(LinuxTimerPALTest, ScheduleRepeatingReturnsValidHandle) {
    std::atomic<int> fireCount{0};

    auto result = timerPal_->scheduleRepeating(
        std::chrono::milliseconds{50},
        [&fireCount]() { fireCount++; }
    );

    EXPECT_TRUE(result.isSuccess());
    EXPECT_NE(result.value(), INVALID_TIMER_HANDLE);

    // Wait for multiple fires
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Cancel the timer
    timerPal_->cancelTimer(result.value());

    EXPECT_GE(fireCount.load(), 2);
}

TEST_F(LinuxTimerPALTest, ScheduleRepeatingFiresMultipleTimes) {
    std::atomic<int> fireCount{0};

    auto result = timerPal_->scheduleRepeating(
        std::chrono::milliseconds{30},
        [&fireCount]() { fireCount++; }
    );

    ASSERT_TRUE(result.isSuccess());

    // Wait for several intervals
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Cancel the timer
    timerPal_->cancelTimer(result.value());

    // Should have fired several times
    EXPECT_GE(fireCount.load(), 4);
    EXPECT_LE(fireCount.load(), 10);
}

TEST_F(LinuxTimerPALTest, CancelRepeatingTimerStopsCallbacks) {
    std::atomic<int> fireCount{0};

    auto result = timerPal_->scheduleRepeating(
        std::chrono::milliseconds{30},
        [&fireCount]() { fireCount++; }
    );

    ASSERT_TRUE(result.isSuccess());

    // Wait for a couple fires
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int countAtCancel = fireCount.load();
    EXPECT_GE(countAtCancel, 1);

    // Cancel the timer
    auto cancelResult = timerPal_->cancelTimer(result.value());
    EXPECT_TRUE(cancelResult.isSuccess());

    // Wait more and ensure no more fires
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Should not have many more fires (allow 1 more due to race)
    EXPECT_LE(fireCount.load(), countAtCancel + 1);
}

// =============================================================================
// Timer Cancellation Tests
// =============================================================================

TEST_F(LinuxTimerPALTest, CancelInvalidHandleReturnsError) {
    auto result = timerPal_->cancelTimer(INVALID_TIMER_HANDLE);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, TimerErrorCode::InvalidHandle);
}

TEST_F(LinuxTimerPALTest, CancelAlreadyCancelledTimerReturnsError) {
    std::atomic<bool> fired{false};

    auto result = timerPal_->scheduleOnce(
        std::chrono::milliseconds{1000},
        [&fired]() { fired = true; }
    );

    ASSERT_TRUE(result.isSuccess());

    // Cancel once - should succeed
    auto cancelResult1 = timerPal_->cancelTimer(result.value());
    EXPECT_TRUE(cancelResult1.isSuccess());

    // Cancel again - should fail
    auto cancelResult2 = timerPal_->cancelTimer(result.value());
    EXPECT_TRUE(cancelResult2.isError());
}

// =============================================================================
// Time Measurement Tests
// =============================================================================

TEST_F(LinuxTimerPALTest, NowReturnsMonotonicallyIncreasingValues) {
    auto t1 = timerPal_->now();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto t2 = timerPal_->now();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto t3 = timerPal_->now();

    EXPECT_LT(t1, t2);
    EXPECT_LT(t2, t3);
}

TEST_F(LinuxTimerPALTest, GetMonotonicMillisReturnsIncreasingValues) {
    uint64_t t1 = timerPal_->getMonotonicMillis();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    uint64_t t2 = timerPal_->getMonotonicMillis();

    EXPECT_GT(t2, t1);
    EXPECT_GE(t2 - t1, 40);  // Allow some tolerance
}

TEST_F(LinuxTimerPALTest, NowAndGetMonotonicMillisAreConsistent) {
    auto t1 = timerPal_->now();
    uint64_t millis1 = timerPal_->getMonotonicMillis();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto t2 = timerPal_->now();
    uint64_t millis2 = timerPal_->getMonotonicMillis();

    // Both should show similar elapsed time
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1);
    int64_t millisElapsed = static_cast<int64_t>(millis2 - millis1);

    EXPECT_NEAR(elapsed.count(), millisElapsed, 20);  // Within 20ms tolerance
}

// =============================================================================
// Multiple Timers Tests
// =============================================================================

TEST_F(LinuxTimerPALTest, MultipleTimersFireIndependently) {
    std::atomic<bool> timer1Fired{false};
    std::atomic<bool> timer2Fired{false};
    std::atomic<bool> timer3Fired{false};

    auto result1 = timerPal_->scheduleOnce(
        std::chrono::milliseconds{30},
        [&timer1Fired]() { timer1Fired = true; }
    );

    auto result2 = timerPal_->scheduleOnce(
        std::chrono::milliseconds{60},
        [&timer2Fired]() { timer2Fired = true; }
    );

    auto result3 = timerPal_->scheduleOnce(
        std::chrono::milliseconds{90},
        [&timer3Fired]() { timer3Fired = true; }
    );

    ASSERT_TRUE(result1.isSuccess());
    ASSERT_TRUE(result2.isSuccess());
    ASSERT_TRUE(result3.isSuccess());

    // Wait for first timer
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(timer1Fired.load());
    EXPECT_FALSE(timer2Fired.load());
    EXPECT_FALSE(timer3Fired.load());

    // Wait for second timer
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_TRUE(timer2Fired.load());
    EXPECT_FALSE(timer3Fired.load());

    // Wait for third timer
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(timer3Fired.load());
}

TEST_F(LinuxTimerPALTest, MixedOneShotAndRepeatingTimers) {
    std::atomic<bool> oneShotFired{false};
    std::atomic<int> repeatingCount{0};

    auto oneShotResult = timerPal_->scheduleOnce(
        std::chrono::milliseconds{75},
        [&oneShotFired]() { oneShotFired = true; }
    );

    auto repeatingResult = timerPal_->scheduleRepeating(
        std::chrono::milliseconds{30},
        [&repeatingCount]() { repeatingCount++; }
    );

    ASSERT_TRUE(oneShotResult.isSuccess());
    ASSERT_TRUE(repeatingResult.isSuccess());

    // Wait
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    timerPal_->cancelTimer(repeatingResult.value());

    EXPECT_TRUE(oneShotFired.load());
    EXPECT_GE(repeatingCount.load(), 4);
}

#endif // defined(__linux__) || defined(__ANDROID__)

} // namespace test
} // namespace pal
} // namespace openrtmp
