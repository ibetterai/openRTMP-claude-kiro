// OpenRTMP - Cross-platform RTMP Server
// Tests for Publisher Limits
//
// Tests cover:
// - Enforce 10 simultaneous publishers on desktop platforms
// - Enforce 3 simultaneous publishers on mobile platforms
// - Return error response when publishing limits reached
// - Track and report current publisher count in metrics
//
// Requirements coverage:
// - Requirement 14.3: Desktop platforms support at least 10 simultaneous publishing streams
// - Requirement 14.4: Mobile platforms support at least 3 simultaneous publishing streams

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <cstdint>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>

#include "openrtmp/core/publisher_limit.hpp"
#include "openrtmp/core/types.hpp"
#include "openrtmp/core/error_codes.hpp"

namespace openrtmp {
namespace core {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class PublisherLimitTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create limiter with small limit for testing
        PublisherLimitConfig config;
        config.maxPublishers = 5;
        limiter_ = std::make_unique<PublisherLimit>(config);
    }

    void TearDown() override {
        limiter_.reset();
    }

    std::unique_ptr<PublisherLimit> limiter_;

    // Helper to acquire multiple publisher slots
    std::vector<PublisherId> acquireMultiple(size_t count) {
        std::vector<PublisherId> ids;
        for (size_t i = 0; i < count; ++i) {
            PublisherId publisherId = static_cast<PublisherId>(1000 + i);
            auto result = limiter_->acquirePublisherSlot(publisherId);
            if (result.isSuccess()) {
                ids.push_back(publisherId);
            }
        }
        return ids;
    }
};

// =============================================================================
// Configuration Tests
// =============================================================================

TEST_F(PublisherLimitTest, DefaultDesktopLimit) {
    // Default desktop limit should be 10 when using platformDefault()
    if (isDesktopPlatform()) {
        PublisherLimitConfig defaultConfig = PublisherLimitConfig::platformDefault();
        EXPECT_EQ(defaultConfig.maxPublishers, DESKTOP_PUBLISHER_LIMIT);
    }
}

TEST_F(PublisherLimitTest, DefaultMobileLimit) {
    // Default mobile limit should be 3 when using platformDefault()
    if (isMobilePlatform()) {
        PublisherLimitConfig defaultConfig = PublisherLimitConfig::platformDefault();
        EXPECT_EQ(defaultConfig.maxPublishers, MOBILE_PUBLISHER_LIMIT);
    }
}

TEST_F(PublisherLimitTest, ConfigurationWithCustomLimit) {
    PublisherLimitConfig config;
    config.maxPublishers = 20;

    PublisherLimit customLimiter(config);
    auto limiterConfig = customLimiter.getConfig();

    EXPECT_EQ(limiterConfig.maxPublishers, 20u);
}

TEST_F(PublisherLimitTest, ZeroMaxPublishersUsesDefault) {
    PublisherLimitConfig config;
    config.maxPublishers = 0;

    // Should use minimum of 1
    PublisherLimit limiter(config);
    auto limiterConfig = limiter.getConfig();

    EXPECT_GE(limiterConfig.maxPublishers, 1u);
}

// =============================================================================
// Acquire Publisher Slot Tests
// =============================================================================

TEST_F(PublisherLimitTest, AcquirePublisherSlotSuccess) {
    PublisherId publisherId = 100;

    auto result = limiter_->acquirePublisherSlot(publisherId);

    EXPECT_TRUE(result.isSuccess());
}

TEST_F(PublisherLimitTest, AcquirePublisherSlotUpdatesCount) {
    PublisherId publisherId = 100;

    EXPECT_EQ(limiter_->getActivePublisherCount(), 0u);

    auto result = limiter_->acquirePublisherSlot(publisherId);
    ASSERT_TRUE(result.isSuccess());

    EXPECT_EQ(limiter_->getActivePublisherCount(), 1u);
}

TEST_F(PublisherLimitTest, DuplicateAcquireFails) {
    PublisherId publisherId = 100;

    auto result1 = limiter_->acquirePublisherSlot(publisherId);
    ASSERT_TRUE(result1.isSuccess());

    auto result2 = limiter_->acquirePublisherSlot(publisherId);
    EXPECT_FALSE(result2.isSuccess());
    EXPECT_EQ(result2.error().code, ErrorCode::AlreadyExists);
}

TEST_F(PublisherLimitTest, MultiplePublishersCanAcquire) {
    auto ids = acquireMultiple(3);

    EXPECT_EQ(ids.size(), 3u);
    EXPECT_EQ(limiter_->getActivePublisherCount(), 3u);
}

// =============================================================================
// Publisher Limit Enforcement Tests (Requirements 14.3, 14.4)
// =============================================================================

TEST_F(PublisherLimitTest, RejectsPublishersWhenLimitReached) {
    // Acquire all available slots (limit is 5)
    auto ids = acquireMultiple(5);
    EXPECT_EQ(ids.size(), 5u);

    // Try to acquire one more - should fail
    PublisherId extraPublisher = 9999;
    auto result = limiter_->acquirePublisherSlot(extraPublisher);

    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.error().code, ErrorCode::StreamLimitReached);
}

TEST_F(PublisherLimitTest, PublisherLimitErrorIncludesMessage) {
    auto ids = acquireMultiple(5);

    PublisherId extraPublisher = 9999;
    auto result = limiter_->acquirePublisherSlot(extraPublisher);

    EXPECT_FALSE(result.isSuccess());
    EXPECT_FALSE(result.error().message.empty());
    // Message should indicate the limit
    EXPECT_NE(result.error().message.find("limit"), std::string::npos);
}

TEST_F(PublisherLimitTest, AcceptsPublishersAfterRelease) {
    // Fill to limit
    auto ids = acquireMultiple(5);
    EXPECT_EQ(ids.size(), 5u);

    // Release one
    limiter_->releasePublisherSlot(ids.back());

    // Should be able to acquire again
    PublisherId newPublisher = 9999;
    auto result = limiter_->acquirePublisherSlot(newPublisher);
    EXPECT_TRUE(result.isSuccess());
}

TEST_F(PublisherLimitTest, DesktopLimitIs10) {
    // Test with desktop limit
    PublisherLimitConfig desktopConfig;
    desktopConfig.maxPublishers = DESKTOP_PUBLISHER_LIMIT;

    PublisherLimit desktopLimiter(desktopConfig);

    // Should be able to acquire 10 publishers
    std::vector<PublisherId> ids;
    for (size_t i = 0; i < 10; ++i) {
        PublisherId publisherId = static_cast<PublisherId>(1000 + i);
        auto result = desktopLimiter.acquirePublisherSlot(publisherId);
        if (result.isSuccess()) {
            ids.push_back(publisherId);
        }
    }

    EXPECT_EQ(ids.size(), 10u);

    // 11th should fail
    PublisherId extraPublisher = 9999;
    auto result = desktopLimiter.acquirePublisherSlot(extraPublisher);
    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.error().code, ErrorCode::StreamLimitReached);
}

TEST_F(PublisherLimitTest, MobileLimitIs3) {
    // Test with mobile limit
    PublisherLimitConfig mobileConfig;
    mobileConfig.maxPublishers = MOBILE_PUBLISHER_LIMIT;

    PublisherLimit mobileLimiter(mobileConfig);

    // Should be able to acquire 3 publishers
    std::vector<PublisherId> ids;
    for (size_t i = 0; i < 3; ++i) {
        PublisherId publisherId = static_cast<PublisherId>(1000 + i);
        auto result = mobileLimiter.acquirePublisherSlot(publisherId);
        if (result.isSuccess()) {
            ids.push_back(publisherId);
        }
    }

    EXPECT_EQ(ids.size(), 3u);

    // 4th should fail
    PublisherId extraPublisher = 9999;
    auto result = mobileLimiter.acquirePublisherSlot(extraPublisher);
    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.error().code, ErrorCode::StreamLimitReached);
}

// =============================================================================
// Release Publisher Slot Tests
// =============================================================================

TEST_F(PublisherLimitTest, ReleasePublisherSlotDecrementsCount) {
    PublisherId publisherId = 100;

    auto result = limiter_->acquirePublisherSlot(publisherId);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(limiter_->getActivePublisherCount(), 1u);

    limiter_->releasePublisherSlot(publisherId);

    EXPECT_EQ(limiter_->getActivePublisherCount(), 0u);
}

TEST_F(PublisherLimitTest, ReleaseNonExistentPublisherIsIgnored) {
    // Release a publisher that was never acquired - should not crash
    limiter_->releasePublisherSlot(999);

    EXPECT_EQ(limiter_->getActivePublisherCount(), 0u);
}

TEST_F(PublisherLimitTest, DoubleReleaseIsIgnored) {
    PublisherId publisherId = 100;

    auto result = limiter_->acquirePublisherSlot(publisherId);
    ASSERT_TRUE(result.isSuccess());

    limiter_->releasePublisherSlot(publisherId);
    limiter_->releasePublisherSlot(publisherId);  // Second release should be ignored

    EXPECT_EQ(limiter_->getActivePublisherCount(), 0u);
}

// =============================================================================
// Check Methods Tests
// =============================================================================

TEST_F(PublisherLimitTest, HasPublisherReturnsTrueForActivePublisher) {
    PublisherId publisherId = 100;

    auto result = limiter_->acquirePublisherSlot(publisherId);
    ASSERT_TRUE(result.isSuccess());

    EXPECT_TRUE(limiter_->hasPublisher(publisherId));
}

TEST_F(PublisherLimitTest, HasPublisherReturnsFalseForInactivePublisher) {
    EXPECT_FALSE(limiter_->hasPublisher(999));
}

TEST_F(PublisherLimitTest, HasPublisherReturnsFalseAfterRelease) {
    PublisherId publisherId = 100;

    auto result = limiter_->acquirePublisherSlot(publisherId);
    ASSERT_TRUE(result.isSuccess());

    limiter_->releasePublisherSlot(publisherId);

    EXPECT_FALSE(limiter_->hasPublisher(publisherId));
}

TEST_F(PublisherLimitTest, CanAcquireReturnsTrueWhenBelowLimit) {
    EXPECT_TRUE(limiter_->canAcquire());

    acquireMultiple(3);

    EXPECT_TRUE(limiter_->canAcquire());
}

TEST_F(PublisherLimitTest, CanAcquireReturnsFalseWhenAtLimit) {
    acquireMultiple(5);

    EXPECT_FALSE(limiter_->canAcquire());
}

TEST_F(PublisherLimitTest, GetRemainingSlots) {
    EXPECT_EQ(limiter_->getRemainingSlots(), 5u);

    // Acquire 3 publishers
    for (size_t i = 0; i < 3; ++i) {
        PublisherId publisherId = static_cast<PublisherId>(100 + i);
        limiter_->acquirePublisherSlot(publisherId);
    }

    EXPECT_EQ(limiter_->getRemainingSlots(), 2u);

    // Acquire 2 more publishers with different IDs
    for (size_t i = 0; i < 2; ++i) {
        PublisherId publisherId = static_cast<PublisherId>(200 + i);
        limiter_->acquirePublisherSlot(publisherId);
    }

    EXPECT_EQ(limiter_->getRemainingSlots(), 0u);
}

// =============================================================================
// Statistics Tests
// =============================================================================

TEST_F(PublisherLimitTest, TracksStatistics) {
    auto stats = limiter_->getStatistics();

    EXPECT_EQ(stats.activePublishers, 0u);
    EXPECT_EQ(stats.peakPublishers, 0u);
    EXPECT_EQ(stats.totalAcquires, 0u);
    EXPECT_EQ(stats.totalReleases, 0u);
    EXPECT_EQ(stats.rejectedAcquires, 0u);
}

TEST_F(PublisherLimitTest, TracksPeakPublishers) {
    // Acquire 4 publishers
    auto ids = acquireMultiple(4);

    // Release 2
    for (size_t i = 0; i < 2; ++i) {
        limiter_->releasePublisherSlot(ids.back());
        ids.pop_back();
    }

    auto stats = limiter_->getStatistics();
    EXPECT_EQ(stats.peakPublishers, 4u);
    EXPECT_EQ(stats.activePublishers, 2u);
}

TEST_F(PublisherLimitTest, TracksRejectedAcquires) {
    // Fill to limit
    acquireMultiple(5);

    // Try to acquire more (should be rejected)
    for (int i = 0; i < 3; ++i) {
        limiter_->acquirePublisherSlot(static_cast<PublisherId>(9000 + i));
    }

    auto stats = limiter_->getStatistics();
    EXPECT_EQ(stats.rejectedAcquires, 3u);
}

TEST_F(PublisherLimitTest, TracksTotalAcquiresAndReleases) {
    auto ids = acquireMultiple(3);

    for (auto& id : ids) {
        limiter_->releasePublisherSlot(id);
    }

    auto stats = limiter_->getStatistics();
    EXPECT_EQ(stats.totalAcquires, 3u);
    EXPECT_EQ(stats.totalReleases, 3u);
}

TEST_F(PublisherLimitTest, ResetStatistics) {
    auto ids = acquireMultiple(3);
    for (auto& id : ids) {
        limiter_->releasePublisherSlot(id);
    }

    limiter_->resetStatistics();

    auto stats = limiter_->getStatistics();
    EXPECT_EQ(stats.peakPublishers, 0u);
    EXPECT_EQ(stats.totalAcquires, 0u);
    EXPECT_EQ(stats.totalReleases, 0u);
    EXPECT_EQ(stats.rejectedAcquires, 0u);
}

// =============================================================================
// Callback Tests
// =============================================================================

TEST_F(PublisherLimitTest, InvokesCallbackOnLimitReached) {
    bool callbackInvoked = false;
    PublisherLimitEvent capturedEvent;

    limiter_->setPublisherLimitCallback([&](const PublisherLimitEvent& event) {
        if (event.type == PublisherLimitEventType::LimitReached) {
            callbackInvoked = true;
            capturedEvent = event;
        }
    });

    // Fill to limit
    acquireMultiple(5);

    // Try to acquire one more - should trigger callback
    limiter_->acquirePublisherSlot(9999);

    EXPECT_TRUE(callbackInvoked);
    EXPECT_EQ(capturedEvent.type, PublisherLimitEventType::LimitReached);
    EXPECT_EQ(capturedEvent.currentCount, 5u);
    EXPECT_EQ(capturedEvent.maxCount, 5u);
}

TEST_F(PublisherLimitTest, InvokesCallbackOnPublisherRejected) {
    bool callbackInvoked = false;
    PublisherLimitEvent capturedEvent;

    limiter_->setPublisherLimitCallback([&](const PublisherLimitEvent& event) {
        if (event.type == PublisherLimitEventType::PublisherRejected) {
            callbackInvoked = true;
            capturedEvent = event;
        }
    });

    acquireMultiple(5);
    limiter_->acquirePublisherSlot(9999);

    EXPECT_TRUE(callbackInvoked);
    EXPECT_EQ(capturedEvent.type, PublisherLimitEventType::PublisherRejected);
}

TEST_F(PublisherLimitTest, InvokesCallbackOnHighWatermark) {
    bool callbackInvoked = false;
    PublisherLimitEvent capturedEvent;

    PublisherLimitConfig config;
    config.maxPublishers = 10;
    config.highWatermarkPercent = 80;  // 80% = 8 publishers

    PublisherLimit limiter(config);
    limiter.setPublisherLimitCallback([&](const PublisherLimitEvent& event) {
        if (event.type == PublisherLimitEventType::HighWatermark) {
            callbackInvoked = true;
            capturedEvent = event;
        }
    });

    // Acquire 8 publishers (80% threshold)
    for (int i = 0; i < 8; ++i) {
        limiter.acquirePublisherSlot(static_cast<PublisherId>(1000 + i));
    }

    EXPECT_TRUE(callbackInvoked);
    EXPECT_EQ(capturedEvent.type, PublisherLimitEventType::HighWatermark);
}

// =============================================================================
// Clear and Lifecycle Tests
// =============================================================================

TEST_F(PublisherLimitTest, ClearReleasesAllPublishers) {
    acquireMultiple(4);
    EXPECT_EQ(limiter_->getActivePublisherCount(), 4u);

    limiter_->clear();

    EXPECT_EQ(limiter_->getActivePublisherCount(), 0u);
}

TEST_F(PublisherLimitTest, ClearPreservesConfiguration) {
    auto configBefore = limiter_->getConfig();

    acquireMultiple(3);
    limiter_->clear();

    auto configAfter = limiter_->getConfig();
    EXPECT_EQ(configAfter.maxPublishers, configBefore.maxPublishers);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST_F(PublisherLimitTest, ConcurrentAcquireAndRelease) {
    PublisherLimitConfig config;
    config.maxPublishers = 50;

    PublisherLimit limiter(config);

    std::atomic<bool> running{true};
    std::atomic<size_t> successCount{0};
    std::atomic<size_t> failCount{0};
    std::vector<std::thread> threads;

    // Multiple acquire/release threads
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&limiter, &running, &successCount, &failCount, i]() {
            PublisherId baseId = static_cast<PublisherId>(i * 1000);
            PublisherId currentId = baseId;
            while (running) {
                auto result = limiter.acquirePublisherSlot(currentId);
                if (result.isSuccess()) {
                    ++successCount;
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    limiter.releasePublisherSlot(currentId);
                } else {
                    ++failCount;
                }
                ++currentId;
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    running = false;

    for (auto& t : threads) {
        t.join();
    }

    // Should have had successful acquires
    EXPECT_GT(successCount.load(), 0u);

    // Final state should be consistent
    EXPECT_EQ(limiter.getActivePublisherCount(), 0u);
}

TEST_F(PublisherLimitTest, NoDataRaceOnStatistics) {
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;

    // Writer threads
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([this, &running, i]() {
            PublisherId baseId = static_cast<PublisherId>(i * 100);
            while (running) {
                auto result = limiter_->acquirePublisherSlot(baseId);
                if (result.isSuccess()) {
                    limiter_->releasePublisherSlot(baseId);
                }
                ++baseId;
            }
        });
    }

    // Reader thread checking statistics
    threads.emplace_back([this, &running]() {
        while (running) {
            auto stats = limiter_->getStatistics();
            // Statistics should always be consistent
            EXPECT_LE(stats.activePublishers, limiter_->getConfig().maxPublishers);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& t : threads) {
        t.join();
    }
}

// =============================================================================
// Platform-Aware Tests
// =============================================================================

TEST(PublisherLimitPlatformTest, PlatformAwareLimits) {
    PublisherLimitConfig config = PublisherLimitConfig::platformDefault();

    if (isDesktopPlatform()) {
        // Desktop: 10 publishers (Requirement 14.3)
        EXPECT_EQ(config.maxPublishers, DESKTOP_PUBLISHER_LIMIT);
    } else if (isMobilePlatform()) {
        // Mobile: 3 publishers (Requirement 14.4)
        EXPECT_EQ(config.maxPublishers, MOBILE_PUBLISHER_LIMIT);
    }
}

TEST(PublisherLimitPlatformTest, CanOverridePlatformDefaults) {
    PublisherLimitConfig config = PublisherLimitConfig::platformDefault();
    config.maxPublishers = 25;  // Override default

    PublisherLimit limiter(config);
    auto limiterConfig = limiter.getConfig();

    EXPECT_EQ(limiterConfig.maxPublishers, 25u);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(PublisherLimitTest, HandlesRapidAcquireRelease) {
    // Rapid acquire/release cycles
    for (int i = 0; i < 1000; ++i) {
        PublisherId publisherId = static_cast<PublisherId>(i);
        auto result = limiter_->acquirePublisherSlot(publisherId);
        if (result.isSuccess()) {
            limiter_->releasePublisherSlot(publisherId);
        }
    }

    EXPECT_EQ(limiter_->getActivePublisherCount(), 0u);
}

TEST_F(PublisherLimitTest, GetActivePublisherIds) {
    auto ids = acquireMultiple(3);

    auto activeIds = limiter_->getActivePublisherIds();

    EXPECT_EQ(activeIds.size(), 3u);
    for (auto& id : ids) {
        EXPECT_TRUE(activeIds.find(id) != activeIds.end());
    }
}

} // namespace test
} // namespace core
} // namespace openrtmp
