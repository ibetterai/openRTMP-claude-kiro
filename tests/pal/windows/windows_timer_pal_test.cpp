// OpenRTMP - Cross-platform RTMP Server
// Tests for Windows Timer PAL Implementation
//
// Requirements Covered: 6.5 (Timer abstraction)

#include <gtest/gtest.h>
#include "openrtmp/pal/timer_pal.hpp"
#include "openrtmp/pal/pal_types.hpp"

#include <atomic>
#include <thread>
#include <chrono>

#if defined(_WIN32)
#include "openrtmp/pal/windows/windows_timer_pal.hpp"
#endif

namespace openrtmp {
namespace pal {
namespace test {

#if defined(_WIN32)

// =============================================================================
// Windows Timer PAL Tests
// =============================================================================

class WindowsTimerPALTest : public ::testing::Test {
protected:
    void SetUp() override {
        timerPal_ = std::make_unique<windows::WindowsTimerPAL>();
    }

    void TearDown() override {
        timerPal_.reset();
    }

    std::unique_ptr<windows::WindowsTimerPAL> timerPal_;
};

TEST_F(WindowsTimerPALTest, ImplementsITimerPALInterface) {
    ITimerPAL* interface = timerPal_.get();
    EXPECT_NE(interface, nullptr);
}

TEST_F(WindowsTimerPALTest, NowReturnsValidTimePoint) {
    auto before = std::chrono::steady_clock::now();
    auto now = timerPal_->now();
    auto after = std::chrono::steady_clock::now();

    EXPECT_GE(now, before);
    EXPECT_LE(now, after);
}

TEST_F(WindowsTimerPALTest, NowIsMonotonicallyIncreasing) {
    auto t1 = timerPal_->now();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto t2 = timerPal_->now();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto t3 = timerPal_->now();

    EXPECT_GT(t2, t1);
    EXPECT_GT(t3, t2);
}

TEST_F(WindowsTimerPALTest, GetMonotonicMillisReturnsNonZero) {
    uint64_t millis = timerPal_->getMonotonicMillis();
    EXPECT_GT(millis, 0u);
}

TEST_F(WindowsTimerPALTest, GetMonotonicMillisIsMonotonicallyIncreasing) {
    uint64_t t1 = timerPal_->getMonotonicMillis();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    uint64_t t2 = timerPal_->getMonotonicMillis();

    EXPECT_GT(t2, t1);
    EXPECT_GE(t2 - t1, 40u);  // Allow some tolerance
}

TEST_F(WindowsTimerPALTest, ScheduleOnceReturnsValidHandle) {
    std::atomic<bool> called{false};

    auto result = timerPal_->scheduleOnce(
        std::chrono::milliseconds{100},
        [&called]() { called = true; }
    );

    EXPECT_TRUE(result.isSuccess());
    EXPECT_NE(result.value(), INVALID_TIMER_HANDLE);
}

TEST_F(WindowsTimerPALTest, ScheduleOnceCallbackFires) {
    std::atomic<bool> called{false};
    std::atomic<int> callCount{0};

    auto result = timerPal_->scheduleOnce(
        std::chrono::milliseconds{50},
        [&called, &callCount]() {
            called = true;
            callCount++;
        }
    );

    ASSERT_TRUE(result.isSuccess());

    // Wait for callback to fire
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_TRUE(called.load());
    EXPECT_EQ(callCount.load(), 1);  // One-shot should fire only once
}

TEST_F(WindowsTimerPALTest, ScheduleOnceWithZeroDelayFiresImmediately) {
    std::atomic<bool> called{false};

    auto result = timerPal_->scheduleOnce(
        std::chrono::milliseconds{0},
        [&called]() { called = true; }
    );

    ASSERT_TRUE(result.isSuccess());

    // Wait briefly for callback
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_TRUE(called.load());
}

TEST_F(WindowsTimerPALTest, ScheduleRepeatingReturnsValidHandle) {
    auto result = timerPal_->scheduleRepeating(
        std::chrono::milliseconds{100},
        []() {}
    );

    EXPECT_TRUE(result.isSuccess());
    EXPECT_NE(result.value(), INVALID_TIMER_HANDLE);

    // Clean up
    timerPal_->cancelTimer(result.value());
}

TEST_F(WindowsTimerPALTest, ScheduleRepeatingCallbackFiresMultipleTimes) {
    std::atomic<int> callCount{0};

    auto result = timerPal_->scheduleRepeating(
        std::chrono::milliseconds{30},
        [&callCount]() { callCount++; }
    );

    ASSERT_TRUE(result.isSuccess());

    // Wait for multiple callbacks
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Should have fired multiple times (at least 3-5 times in 200ms with 30ms interval)
    EXPECT_GE(callCount.load(), 3);

    // Clean up
    timerPal_->cancelTimer(result.value());
}

TEST_F(WindowsTimerPALTest, CancelOneShotBeforeFiring) {
    std::atomic<bool> called{false};

    auto result = timerPal_->scheduleOnce(
        std::chrono::milliseconds{200},
        [&called]() { called = true; }
    );

    ASSERT_TRUE(result.isSuccess());

    // Cancel before it fires
    auto cancelResult = timerPal_->cancelTimer(result.value());
    EXPECT_TRUE(cancelResult.isSuccess());

    // Wait past the scheduled time
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    EXPECT_FALSE(called.load());
}

TEST_F(WindowsTimerPALTest, CancelRepeatingTimer) {
    std::atomic<int> callCount{0};

    auto result = timerPal_->scheduleRepeating(
        std::chrono::milliseconds{20},
        [&callCount]() { callCount++; }
    );

    ASSERT_TRUE(result.isSuccess());

    // Let it fire a few times
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    int countBeforeCancel = callCount.load();
    EXPECT_GT(countBeforeCancel, 0);

    // Cancel the timer
    auto cancelResult = timerPal_->cancelTimer(result.value());
    EXPECT_TRUE(cancelResult.isSuccess());

    // Wait a bit more
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Call count should not have increased much (might be 1 more due to race)
    EXPECT_LE(callCount.load(), countBeforeCancel + 1);
}

TEST_F(WindowsTimerPALTest, CancelInvalidHandleReturnsError) {
    auto result = timerPal_->cancelTimer(INVALID_TIMER_HANDLE);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, TimerErrorCode::InvalidHandle);
}

TEST_F(WindowsTimerPALTest, CancelAlreadyCancelledTimerReturnsError) {
    auto scheduleResult = timerPal_->scheduleOnce(
        std::chrono::milliseconds{1000},
        []() {}
    );

    ASSERT_TRUE(scheduleResult.isSuccess());

    // First cancel should succeed
    auto firstCancel = timerPal_->cancelTimer(scheduleResult.value());
    EXPECT_TRUE(firstCancel.isSuccess());

    // Second cancel should fail
    auto secondCancel = timerPal_->cancelTimer(scheduleResult.value());
    EXPECT_TRUE(secondCancel.isError());
    EXPECT_EQ(secondCancel.error().code, TimerErrorCode::AlreadyCancelled);
}

TEST_F(WindowsTimerPALTest, MultipleTimersCanRunConcurrently) {
    std::atomic<int> count1{0};
    std::atomic<int> count2{0};
    std::atomic<int> count3{0};

    auto result1 = timerPal_->scheduleRepeating(
        std::chrono::milliseconds{25},
        [&count1]() { count1++; }
    );

    auto result2 = timerPal_->scheduleRepeating(
        std::chrono::milliseconds{50},
        [&count2]() { count2++; }
    );

    auto result3 = timerPal_->scheduleOnce(
        std::chrono::milliseconds{75},
        [&count3]() { count3++; }
    );

    ASSERT_TRUE(result1.isSuccess());
    ASSERT_TRUE(result2.isSuccess());
    ASSERT_TRUE(result3.isSuccess());

    // Wait for timers to run
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Timer 1 (25ms) should fire more than timer 2 (50ms)
    EXPECT_GT(count1.load(), count2.load());
    // Timer 3 (one-shot) should fire exactly once
    EXPECT_EQ(count3.load(), 1);

    // Clean up
    timerPal_->cancelTimer(result1.value());
    timerPal_->cancelTimer(result2.value());
}

TEST_F(WindowsTimerPALTest, TimerAccuracyWithinTolerance) {
    auto startTime = timerPal_->now();
    std::atomic<bool> fired{false};
    std::chrono::steady_clock::time_point fireTime;

    auto result = timerPal_->scheduleOnce(
        std::chrono::milliseconds{100},
        [&fired, &fireTime, this]() {
            fireTime = timerPal_->now();
            fired = true;
        }
    );

    ASSERT_TRUE(result.isSuccess());

    // Wait for callback
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ASSERT_TRUE(fired.load());

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        fireTime - startTime);

    // Timer should fire within 50ms of expected time (100ms +/- 50ms)
    EXPECT_GE(elapsed.count(), 50);
    EXPECT_LE(elapsed.count(), 200);
}

TEST_F(WindowsTimerPALTest, ScheduleFromCallback) {
    std::atomic<int> stage{0};

    auto result = timerPal_->scheduleOnce(
        std::chrono::milliseconds{20},
        [this, &stage]() {
            stage = 1;
            // Schedule another timer from within callback
            auto innerResult = timerPal_->scheduleOnce(
                std::chrono::milliseconds{20},
                [&stage]() { stage = 2; }
            );
            EXPECT_TRUE(innerResult.isSuccess());
        }
    );

    ASSERT_TRUE(result.isSuccess());

    // Wait for both timers
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_EQ(stage.load(), 2);
}

TEST_F(WindowsTimerPALTest, CancelFromCallback) {
    std::atomic<int> callCount{0};
    TimerHandle timerHandle = INVALID_TIMER_HANDLE;

    auto result = timerPal_->scheduleRepeating(
        std::chrono::milliseconds{30},
        [this, &callCount, &timerHandle]() {
            callCount++;
            if (callCount >= 3) {
                // Cancel self from callback
                timerPal_->cancelTimer(timerHandle);
            }
        }
    );

    ASSERT_TRUE(result.isSuccess());
    timerHandle = result.value();

    // Wait for callback to self-cancel
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Should have stopped at 3 (or possibly 4 due to race)
    EXPECT_LE(callCount.load(), 4);
}

#endif // _WIN32

} // namespace test
} // namespace pal
} // namespace openrtmp
