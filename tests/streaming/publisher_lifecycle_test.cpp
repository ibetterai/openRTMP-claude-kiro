// OpenRTMP - Cross-platform RTMP Server
// Tests for Publisher Lifecycle Management
//
// Tests cover:
// - Timestamp continuity monitoring and gap detection (> 1 second)
// - Publisher disconnect detection within 5 seconds
// - Subscriber notification on stream unavailability
// - Stream statistics tracking (bitrate, frame counts)
// - Graceful unpublish with resource cleanup
//
// Requirements coverage:
// - Requirement 4.5: Mark stream unavailable within 5 seconds of publisher disconnect
// - Requirement 4.7: Validate timestamp continuity, log gaps > 1 second

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <cstdint>
#include <functional>
#include <chrono>
#include <thread>
#include <atomic>

#include "openrtmp/streaming/publisher_lifecycle.hpp"
#include "openrtmp/streaming/media_handler.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace streaming {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class PublisherLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        lifecycle_ = std::make_unique<PublisherLifecycle>();
    }

    void TearDown() override {
        lifecycle_.reset();
    }

    std::unique_ptr<PublisherLifecycle> lifecycle_;

    // Helper to create a video media message
    MediaMessage createVideoMessage(uint32_t timestamp, bool isKeyframe = false) {
        MediaMessage msg;
        msg.type = MediaType::Video;
        msg.timestamp = timestamp;
        msg.payload = {0x17, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};  // AVC NALU
        msg.isKeyframe = isKeyframe;
        return msg;
    }

    // Helper to create an audio media message
    MediaMessage createAudioMessage(uint32_t timestamp) {
        MediaMessage msg;
        msg.type = MediaType::Audio;
        msg.timestamp = timestamp;
        msg.payload = {0xAF, 0x01, 0x21, 0x00, 0x49, 0x90};  // AAC raw data
        msg.isKeyframe = true;  // Audio is always treated as keyframe
        return msg;
    }

    // Helper to simulate time passing
    void simulateTime(std::chrono::milliseconds duration) {
        std::this_thread::sleep_for(duration);
    }
};

// =============================================================================
// Timestamp Continuity Monitoring Tests (Requirement 4.7)
// =============================================================================

TEST_F(PublisherLifecycleTest, DetectsTimestampGapExceedingOneSecond) {
    std::vector<TimestampGapInfo> detectedGaps;

    lifecycle_->setTimestampGapCallback([&](const TimestampGapInfo& gap) {
        detectedGaps.push_back(gap);
    });

    lifecycle_->start();

    // Send first frame at timestamp 0
    auto msg1 = createVideoMessage(0, true);
    lifecycle_->onMediaReceived(msg1);

    // Send second frame with > 1 second gap (1500ms gap)
    auto msg2 = createVideoMessage(1500);
    lifecycle_->onMediaReceived(msg2);

    ASSERT_EQ(detectedGaps.size(), 1u);
    EXPECT_EQ(detectedGaps[0].previousTimestamp, 0u);
    EXPECT_EQ(detectedGaps[0].currentTimestamp, 1500u);
    EXPECT_EQ(detectedGaps[0].gapMs, 1500u);
}

TEST_F(PublisherLifecycleTest, DoesNotDetectGapUnderOneSecond) {
    std::vector<TimestampGapInfo> detectedGaps;

    lifecycle_->setTimestampGapCallback([&](const TimestampGapInfo& gap) {
        detectedGaps.push_back(gap);
    });

    lifecycle_->start();

    // Send frames with normal timing (< 1 second gap)
    lifecycle_->onMediaReceived(createVideoMessage(0, true));
    lifecycle_->onMediaReceived(createVideoMessage(33));   // 33ms = ~30fps
    lifecycle_->onMediaReceived(createVideoMessage(66));
    lifecycle_->onMediaReceived(createVideoMessage(100));
    lifecycle_->onMediaReceived(createVideoMessage(999));  // Just under 1 second

    EXPECT_EQ(detectedGaps.size(), 0u);
}

TEST_F(PublisherLifecycleTest, DetectsExactlyOneSecondGap) {
    std::vector<TimestampGapInfo> detectedGaps;

    lifecycle_->setTimestampGapCallback([&](const TimestampGapInfo& gap) {
        detectedGaps.push_back(gap);
    });

    lifecycle_->start();

    // Send first frame at timestamp 0
    lifecycle_->onMediaReceived(createVideoMessage(0, true));

    // Send second frame with exactly 1001ms gap (just over 1 second)
    lifecycle_->onMediaReceived(createVideoMessage(1001));

    // Should detect gap since > 1000ms
    ASSERT_EQ(detectedGaps.size(), 1u);
    EXPECT_EQ(detectedGaps[0].gapMs, 1001u);
}

TEST_F(PublisherLifecycleTest, TracksAudioAndVideoTimestampsSeparately) {
    std::vector<TimestampGapInfo> detectedGaps;

    lifecycle_->setTimestampGapCallback([&](const TimestampGapInfo& gap) {
        detectedGaps.push_back(gap);
    });

    lifecycle_->start();

    // Send video at 0
    lifecycle_->onMediaReceived(createVideoMessage(0, true));

    // Send audio at 500
    lifecycle_->onMediaReceived(createAudioMessage(500));

    // Send video at 2000 (2000ms gap from last video)
    lifecycle_->onMediaReceived(createVideoMessage(2000));

    // Should detect video gap
    ASSERT_GE(detectedGaps.size(), 1u);
    EXPECT_EQ(detectedGaps[0].mediaType, MediaType::Video);
    EXPECT_EQ(detectedGaps[0].gapMs, 2000u);
}

TEST_F(PublisherLifecycleTest, HandlesTimestampWrapAround) {
    std::vector<TimestampGapInfo> detectedGaps;

    lifecycle_->setTimestampGapCallback([&](const TimestampGapInfo& gap) {
        detectedGaps.push_back(gap);
    });

    lifecycle_->start();

    // Send frame near max timestamp
    lifecycle_->onMediaReceived(createVideoMessage(0xFFFFFF00, true));

    // Send frame after wraparound (small timestamp)
    // This should NOT be treated as a huge gap
    lifecycle_->onMediaReceived(createVideoMessage(100));

    // No gap should be detected for normal wraparound
    EXPECT_EQ(detectedGaps.size(), 0u);
}

// =============================================================================
// Publisher Disconnect Detection Tests (Requirement 4.5)
// =============================================================================

TEST_F(PublisherLifecycleTest, MarksStreamUnavailableWithinFiveSeconds) {
    // This test validates that the stream is marked unavailable within 5 seconds
    // per Requirement 4.5. Since our implementation notifies synchronously (which is
    // well within 5 seconds), we verify the callback is invoked immediately.

    std::atomic<bool> unavailableNotified{false};

    lifecycle_->setStreamUnavailableCallback([&unavailableNotified]() {
        unavailableNotified = true;
    });

    lifecycle_->start();

    // Send some media
    lifecycle_->onMediaReceived(createVideoMessage(0, true));

    // Simulate unexpected disconnect
    auto disconnectTime = std::chrono::steady_clock::now();
    lifecycle_->onPublisherDisconnected(DisconnectReason::Unexpected);
    auto notifyTime = std::chrono::steady_clock::now();

    // Should be notified immediately (within same call)
    ASSERT_TRUE(unavailableNotified.load());

    // Verify it happened within 5 seconds (should be immediate, but we allow up to 5s per spec)
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(notifyTime - disconnectTime);
    EXPECT_LE(elapsed.count(), 5);

    // Cleanup
    lifecycle_->stop();
}

TEST_F(PublisherLifecycleTest, ReportsDisconnectReasonInCallback) {
    DisconnectReason reportedReason = DisconnectReason::Graceful;

    lifecycle_->setDisconnectCallback([&](DisconnectReason reason) {
        reportedReason = reason;
    });

    lifecycle_->start();

    lifecycle_->onPublisherDisconnected(DisconnectReason::Unexpected);

    EXPECT_EQ(reportedReason, DisconnectReason::Unexpected);
}

TEST_F(PublisherLifecycleTest, GracefulDisconnectDoesNotTriggerUnavailable) {
    std::atomic<bool> unavailableNotified{false};

    lifecycle_->setStreamUnavailableCallback([&]() {
        unavailableNotified = true;
    });

    lifecycle_->start();

    // Send some media
    lifecycle_->onMediaReceived(createVideoMessage(0, true));

    // Graceful disconnect (unpublish)
    lifecycle_->onPublisherDisconnected(DisconnectReason::Graceful);

    // Wait a bit to ensure no callback
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    EXPECT_FALSE(unavailableNotified.load());
}

// =============================================================================
// Subscriber Notification Tests
// =============================================================================

TEST_F(PublisherLifecycleTest, NotifiesSubscribersOnStreamUnavailable) {
    std::vector<SubscriberId> notifiedSubscribers;

    lifecycle_->setSubscriberNotificationCallback(
        [&](SubscriberId subId, StreamNotification notification) {
            if (notification == StreamNotification::StreamUnavailable) {
                notifiedSubscribers.push_back(subId);
            }
        });

    lifecycle_->start();

    // Add some subscribers
    lifecycle_->addSubscriber(101);
    lifecycle_->addSubscriber(102);
    lifecycle_->addSubscriber(103);

    // Send some media
    lifecycle_->onMediaReceived(createVideoMessage(0, true));

    // Unexpected disconnect - subscribers should be notified synchronously
    lifecycle_->onPublisherDisconnected(DisconnectReason::Unexpected);

    // All subscribers should be notified
    ASSERT_EQ(notifiedSubscribers.size(), 3u);
    EXPECT_NE(std::find(notifiedSubscribers.begin(), notifiedSubscribers.end(), 101),
              notifiedSubscribers.end());
    EXPECT_NE(std::find(notifiedSubscribers.begin(), notifiedSubscribers.end(), 102),
              notifiedSubscribers.end());
    EXPECT_NE(std::find(notifiedSubscribers.begin(), notifiedSubscribers.end(), 103),
              notifiedSubscribers.end());

    lifecycle_->stop();
}

TEST_F(PublisherLifecycleTest, NotifiesSubscribersOnStreamEnd) {
    std::vector<SubscriberId> notifiedSubscribers;

    lifecycle_->setSubscriberNotificationCallback(
        [&](SubscriberId subId, StreamNotification notification) {
            if (notification == StreamNotification::StreamEnded) {
                notifiedSubscribers.push_back(subId);
            }
        });

    lifecycle_->start();

    lifecycle_->addSubscriber(101);
    lifecycle_->addSubscriber(102);

    // Graceful unpublish
    lifecycle_->onPublisherDisconnected(DisconnectReason::Graceful);

    // Should immediately notify
    ASSERT_EQ(notifiedSubscribers.size(), 2u);
}

// =============================================================================
// Stream Statistics Tests
// =============================================================================

TEST_F(PublisherLifecycleTest, TracksVideoFrameCount) {
    lifecycle_->start();

    // Send keyframe and some inter frames
    lifecycle_->onMediaReceived(createVideoMessage(0, true));
    lifecycle_->onMediaReceived(createVideoMessage(33, false));
    lifecycle_->onMediaReceived(createVideoMessage(66, false));
    lifecycle_->onMediaReceived(createVideoMessage(100, true));
    lifecycle_->onMediaReceived(createVideoMessage(133, false));

    auto stats = lifecycle_->getStatistics();

    EXPECT_EQ(stats.videoFrameCount, 5u);
    EXPECT_EQ(stats.keyframeCount, 2u);
}

TEST_F(PublisherLifecycleTest, TracksAudioFrameCount) {
    lifecycle_->start();

    lifecycle_->onMediaReceived(createAudioMessage(0));
    lifecycle_->onMediaReceived(createAudioMessage(23));
    lifecycle_->onMediaReceived(createAudioMessage(46));

    auto stats = lifecycle_->getStatistics();

    EXPECT_EQ(stats.audioFrameCount, 3u);
}

TEST_F(PublisherLifecycleTest, TracksTotalBytesReceived) {
    lifecycle_->start();

    auto videoMsg = createVideoMessage(0, true);
    auto audioMsg = createAudioMessage(33);

    lifecycle_->onMediaReceived(videoMsg);
    lifecycle_->onMediaReceived(audioMsg);

    auto stats = lifecycle_->getStatistics();

    size_t expectedBytes = videoMsg.payload.size() + audioMsg.payload.size();
    EXPECT_EQ(stats.totalBytesReceived, expectedBytes);
}

TEST_F(PublisherLifecycleTest, CalculatesBitrate) {
    lifecycle_->start();

    // Send data over the bitrate calculation window (2 seconds)
    MediaMessage msg;
    msg.type = MediaType::Video;
    msg.timestamp = 0;
    msg.payload.resize(1000, 0x00);
    msg.isKeyframe = true;

    lifecycle_->onMediaReceived(msg);

    // Wait for the bitrate window (2 seconds)
    simulateTime(std::chrono::milliseconds(2100));

    // Send another message to trigger bitrate calculation
    msg.timestamp = 2100;
    msg.isKeyframe = false;
    lifecycle_->onMediaReceived(msg);

    auto stats = lifecycle_->getStatistics();

    // Bitrate should be calculated after the window
    // 2000 bytes over ~2 seconds = ~8000 bits/s (but may vary)
    EXPECT_GT(stats.currentBitrateBps, 0u);
}

TEST_F(PublisherLifecycleTest, TracksStreamDuration) {
    lifecycle_->start();

    // Send first message
    lifecycle_->onMediaReceived(createVideoMessage(0, true));

    // Wait some time
    simulateTime(std::chrono::milliseconds(500));

    // Send another message to update duration
    lifecycle_->onMediaReceived(createVideoMessage(500, false));

    auto stats = lifecycle_->getStatistics();

    // Duration should be at least 500ms (the time we slept)
    // Note: Duration is updated on each media receive, so it should reflect real elapsed time
    EXPECT_GE(stats.durationMs, 500u);
}

// =============================================================================
// Graceful Unpublish and Resource Cleanup Tests
// =============================================================================

TEST_F(PublisherLifecycleTest, GracefulUnpublishClearsState) {
    lifecycle_->start();

    lifecycle_->onMediaReceived(createVideoMessage(0, true));
    lifecycle_->onMediaReceived(createVideoMessage(33));

    lifecycle_->addSubscriber(101);

    // Graceful unpublish
    lifecycle_->stop();

    // State should be cleared
    EXPECT_FALSE(lifecycle_->isActive());
    EXPECT_EQ(lifecycle_->getSubscriberCount(), 0u);
}

TEST_F(PublisherLifecycleTest, UnpublishNotifiesAllSubscribers) {
    std::vector<SubscriberId> notifiedSubscribers;

    lifecycle_->setSubscriberNotificationCallback(
        [&](SubscriberId subId, StreamNotification notification) {
            if (notification == StreamNotification::StreamEnded) {
                notifiedSubscribers.push_back(subId);
            }
        });

    lifecycle_->start();

    lifecycle_->addSubscriber(101);
    lifecycle_->addSubscriber(102);

    lifecycle_->stop();

    EXPECT_EQ(notifiedSubscribers.size(), 2u);
}

TEST_F(PublisherLifecycleTest, ResetStatisticsOnRestart) {
    lifecycle_->start();

    lifecycle_->onMediaReceived(createVideoMessage(0, true));
    lifecycle_->onMediaReceived(createVideoMessage(33));
    lifecycle_->onMediaReceived(createVideoMessage(66));

    auto statsBefore = lifecycle_->getStatistics();
    EXPECT_EQ(statsBefore.videoFrameCount, 3u);

    lifecycle_->stop();
    lifecycle_->start();

    auto statsAfter = lifecycle_->getStatistics();
    EXPECT_EQ(statsAfter.videoFrameCount, 0u);
    EXPECT_EQ(statsAfter.totalBytesReceived, 0u);
}

TEST_F(PublisherLifecycleTest, RemoveSubscriberDuringUnpublish) {
    lifecycle_->start();

    lifecycle_->addSubscriber(101);
    lifecycle_->addSubscriber(102);

    EXPECT_EQ(lifecycle_->getSubscriberCount(), 2u);

    lifecycle_->removeSubscriber(101);

    EXPECT_EQ(lifecycle_->getSubscriberCount(), 1u);

    lifecycle_->stop();

    EXPECT_EQ(lifecycle_->getSubscriberCount(), 0u);
}

// =============================================================================
// Edge Cases and Error Handling
// =============================================================================

TEST_F(PublisherLifecycleTest, IgnoresMediaWhenNotStarted) {
    // Don't call start()

    lifecycle_->onMediaReceived(createVideoMessage(0, true));
    lifecycle_->onMediaReceived(createVideoMessage(33));

    auto stats = lifecycle_->getStatistics();

    EXPECT_EQ(stats.videoFrameCount, 0u);
}

TEST_F(PublisherLifecycleTest, HandlesMultipleDisconnectCalls) {
    std::atomic<int> disconnectCount{0};

    lifecycle_->setDisconnectCallback([&](DisconnectReason) {
        disconnectCount++;
    });

    lifecycle_->start();

    lifecycle_->onPublisherDisconnected(DisconnectReason::Unexpected);
    lifecycle_->onPublisherDisconnected(DisconnectReason::Unexpected);
    lifecycle_->onPublisherDisconnected(DisconnectReason::Unexpected);

    // Should only notify once
    EXPECT_EQ(disconnectCount.load(), 1);
}

TEST_F(PublisherLifecycleTest, ThreadSafeMediaProcessing) {
    lifecycle_->start();

    std::atomic<int> processedCount{0};
    const int numThreads = 4;
    const int messagesPerThread = 100;

    std::vector<std::thread> threads;

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < messagesPerThread; ++i) {
                auto msg = createVideoMessage(static_cast<uint32_t>(t * 1000 + i), i == 0);
                lifecycle_->onMediaReceived(msg);
                processedCount++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto stats = lifecycle_->getStatistics();

    // All messages should be counted
    EXPECT_EQ(stats.videoFrameCount, static_cast<uint64_t>(numThreads * messagesPerThread));
}

} // namespace test
} // namespace streaming
} // namespace openrtmp
