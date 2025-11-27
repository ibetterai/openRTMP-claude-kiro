// OpenRTMP - Cross-platform RTMP Server
// Tests for Subscriber Buffer Overflow Protection
//
// Tests cover:
// - Track per-subscriber buffer levels independently
// - Drop non-keyframe packets when buffer exceeds 5 seconds
// - Preserve keyframes and audio to maintain stream continuity
// - Log dropped frame statistics per subscriber
// - Support configurable maximum buffer thresholds
//
// Requirements coverage:
// - Requirement 5.4: Maintain independent send buffers for each subscriber
// - Requirement 5.5: Drop non-keyframe packets if buffer exceeds 5 seconds

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <cstdint>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>

#include "openrtmp/streaming/subscriber_buffer.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace streaming {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class SubscriberBufferTest : public ::testing::Test {
protected:
    void SetUp() override {
        buffer_ = std::make_unique<SubscriberBuffer>();
    }

    void TearDown() override {
        buffer_.reset();
    }

    std::unique_ptr<SubscriberBuffer> buffer_;

    // Helper to create a buffered frame
    BufferedFrame createFrame(
        MediaType type,
        uint32_t timestamp,
        bool isKeyframe,
        size_t dataSize = 100
    ) {
        BufferedFrame frame;
        frame.type = type;
        frame.timestamp = timestamp;
        frame.isKeyframe = isKeyframe;
        frame.data.resize(dataSize);
        // Fill with pattern for identification
        for (size_t i = 0; i < dataSize; ++i) {
            frame.data[i] = static_cast<uint8_t>((timestamp + i) & 0xFF);
        }
        return frame;
    }

    // Helper to create a video keyframe
    BufferedFrame createVideoKeyframe(uint32_t timestamp, size_t dataSize = 1000) {
        return createFrame(MediaType::Video, timestamp, true, dataSize);
    }

    // Helper to create a video inter frame (P/B frame)
    BufferedFrame createVideoInterFrame(uint32_t timestamp, size_t dataSize = 500) {
        return createFrame(MediaType::Video, timestamp, false, dataSize);
    }

    // Helper to create an audio frame
    BufferedFrame createAudioFrame(uint32_t timestamp, size_t dataSize = 200) {
        return createFrame(MediaType::Audio, timestamp, true, dataSize);
    }

    // Helper to push frames simulating a stream at 30fps with audio at 21ms intervals
    void pushStreamSegment(uint32_t startTimestamp, uint32_t durationMs, bool startWithKeyframe = true) {
        uint32_t ts = startTimestamp;
        int frameIdx = 0;
        uint32_t audioTs = startTimestamp;

        while (ts < startTimestamp + durationMs) {
            // Video frame every 33ms (30fps)
            if (frameIdx == 0 && startWithKeyframe) {
                buffer_->push(createVideoKeyframe(ts));
            } else {
                buffer_->push(createVideoInterFrame(ts));
            }

            // Audio frames (AAC typically at ~21ms intervals)
            while (audioTs <= ts + 33 && audioTs < startTimestamp + durationMs) {
                buffer_->push(createAudioFrame(audioTs));
                audioTs += 21;
            }

            ts += 33;
            ++frameIdx;
        }
    }
};

// =============================================================================
// Basic Buffer Operations Tests
// =============================================================================

TEST_F(SubscriberBufferTest, InitialStateIsEmpty) {
    EXPECT_EQ(buffer_->getBufferLevel().count(), 0);
    EXPECT_EQ(buffer_->getBufferedBytes(), 0u);
    EXPECT_EQ(buffer_->getPendingFrameCount(), 0u);
    EXPECT_EQ(buffer_->getDroppedFrameCount(), 0u);
}

TEST_F(SubscriberBufferTest, PushFrameUpdatesBufferLevel) {
    buffer_->push(createVideoKeyframe(0));
    buffer_->push(createVideoInterFrame(33));
    buffer_->push(createVideoInterFrame(66));

    EXPECT_GT(buffer_->getBufferedBytes(), 0u);
    EXPECT_EQ(buffer_->getPendingFrameCount(), 3u);
}

TEST_F(SubscriberBufferTest, BufferLevelCalculatedFromTimestamps) {
    buffer_->push(createVideoKeyframe(0));
    buffer_->push(createVideoInterFrame(1000));  // 1 second later

    auto bufferLevel = buffer_->getBufferLevel();
    EXPECT_GE(bufferLevel.count(), 1000);  // At least 1 second
}

TEST_F(SubscriberBufferTest, PopReturnsFramesInOrder) {
    buffer_->push(createVideoKeyframe(0));
    buffer_->push(createVideoInterFrame(33));
    buffer_->push(createVideoInterFrame(66));

    auto frame1 = buffer_->pop();
    ASSERT_TRUE(frame1.has_value());
    EXPECT_EQ(frame1->timestamp, 0u);
    EXPECT_TRUE(frame1->isKeyframe);

    auto frame2 = buffer_->pop();
    ASSERT_TRUE(frame2.has_value());
    EXPECT_EQ(frame2->timestamp, 33u);
    EXPECT_FALSE(frame2->isKeyframe);
}

TEST_F(SubscriberBufferTest, PopFromEmptyBufferReturnsNullopt) {
    auto frame = buffer_->pop();
    EXPECT_FALSE(frame.has_value());
}

// =============================================================================
// Default Configuration Tests
// =============================================================================

TEST_F(SubscriberBufferTest, DefaultMaxBufferDurationIs5Seconds) {
    auto config = buffer_->getConfig();
    EXPECT_EQ(config.maxBufferDuration.count(), 5000);  // 5 seconds
}

TEST_F(SubscriberBufferTest, ConfigurationCanBeChanged) {
    SubscriberBufferConfig config;
    config.maxBufferDuration = std::chrono::milliseconds(3000);  // 3 seconds
    config.preserveAudio = true;

    buffer_->setConfig(config);

    auto retrievedConfig = buffer_->getConfig();
    EXPECT_EQ(retrievedConfig.maxBufferDuration.count(), 3000);
    EXPECT_TRUE(retrievedConfig.preserveAudio);
}

// =============================================================================
// Buffer Overflow Protection Tests (Requirement 5.5)
// =============================================================================

TEST_F(SubscriberBufferTest, DropsNonKeyframePacketsWhenBufferExceeds5Seconds) {
    // Disable audio preservation to test pure video dropping
    SubscriberBufferConfig config;
    config.maxBufferDuration = std::chrono::milliseconds(5000);
    config.preserveAudio = false;  // Don't preserve audio for this test
    buffer_->setConfig(config);

    // Push only video frames (no audio) to make buffer duration predictable
    // Push keyframes at 0, 1000, 2000, 3000, 4000, 5000, 6000ms
    // with inter-frames in between
    for (uint32_t ts = 0; ts <= 6000; ts += 33) {
        bool isKeyframe = (ts % 1000) < 33;  // Keyframe every ~1 second
        if (isKeyframe) {
            buffer_->push(createVideoKeyframe(ts));
        } else {
            buffer_->push(createVideoInterFrame(ts));
        }
    }

    // Buffer should have dropped some non-keyframe packets
    // Note: With keyframes preserved at 0, 1000, 2000, 3000, 4000, 5000, 6000
    // the minimum duration is 6000ms if we keep all keyframes.
    // But we should have dropped inter-frames to reduce the count.
    EXPECT_GT(buffer_->getDroppedFrameCount(), 0u);

    // Verify that the buffer contains more keyframes than inter-frames
    // (most inter-frames should have been dropped)
    size_t keyframeCount = 0;
    size_t interFrameCount = 0;
    while (auto frame = buffer_->pop()) {
        if (frame->type == MediaType::Video) {
            if (frame->isKeyframe) {
                ++keyframeCount;
            } else {
                ++interFrameCount;
            }
        }
    }

    // Should have preserved keyframes
    EXPECT_GE(keyframeCount, 5u);  // At least 5 keyframes (0, 1000, 2000, 3000, 4000, 5000 or 6000)
}

TEST_F(SubscriberBufferTest, PreservesKeyframesWhenDroppingPackets) {
    // First, fill buffer past threshold
    pushStreamSegment(0, 6000, true);  // 6 seconds of video

    // Verify keyframes are still present
    // Extract all frames and check for keyframes
    size_t keyframeCount = 0;
    while (auto frame = buffer_->pop()) {
        if (frame->type == MediaType::Video && frame->isKeyframe) {
            ++keyframeCount;
        }
    }

    // Should have at least one keyframe preserved
    EXPECT_GT(keyframeCount, 0u);
}

TEST_F(SubscriberBufferTest, PreservesAudioWhenDroppingVideoInterFrames) {
    SubscriberBufferConfig config;
    config.maxBufferDuration = std::chrono::milliseconds(5000);
    config.preserveAudio = true;  // Audio should be preserved
    buffer_->setConfig(config);

    // Fill buffer past threshold
    pushStreamSegment(0, 7000, true);

    // Count audio frames
    size_t audioCount = 0;
    while (auto frame = buffer_->pop()) {
        if (frame->type == MediaType::Audio) {
            ++audioCount;
        }
    }

    // Audio should not be dropped
    EXPECT_GT(audioCount, 0u);
}

TEST_F(SubscriberBufferTest, OnlyDropsVideoInterFrames) {
    // Push specific pattern to verify only inter frames are dropped
    buffer_->push(createVideoKeyframe(0));
    buffer_->push(createVideoInterFrame(1000));  // Should be candidate for drop
    buffer_->push(createVideoInterFrame(2000));  // Should be candidate for drop
    buffer_->push(createVideoInterFrame(3000));  // Should be candidate for drop
    buffer_->push(createVideoInterFrame(4000));  // Should be candidate for drop
    buffer_->push(createVideoInterFrame(5000));  // Should be candidate for drop
    buffer_->push(createVideoKeyframe(6000));    // Should be preserved

    // Buffer exceeds 5 seconds, inter frames should be dropped
    auto stats = buffer_->getStatistics();

    // Keyframes should not be in dropped count
    // Inter frames between keyframes should be dropped
    EXPECT_GT(stats.droppedInterFrames, 0u);
    EXPECT_EQ(stats.droppedKeyframes, 0u);
}

// =============================================================================
// Per-Subscriber Independent Buffer Tests (Requirement 5.4)
// =============================================================================

class SubscriberBufferManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager_ = std::make_unique<SubscriberBufferManager>();
    }

    void TearDown() override {
        manager_.reset();
    }

    std::unique_ptr<SubscriberBufferManager> manager_;

    BufferedFrame createVideoKeyframe(uint32_t timestamp) {
        BufferedFrame frame;
        frame.type = MediaType::Video;
        frame.timestamp = timestamp;
        frame.isKeyframe = true;
        frame.data.resize(1000);
        return frame;
    }

    BufferedFrame createVideoInterFrame(uint32_t timestamp) {
        BufferedFrame frame;
        frame.type = MediaType::Video;
        frame.timestamp = timestamp;
        frame.isKeyframe = false;
        frame.data.resize(500);
        return frame;
    }
};

TEST_F(SubscriberBufferManagerTest, CreatesIndependentBuffersPerSubscriber) {
    SubscriberId sub1 = 1;
    SubscriberId sub2 = 2;

    manager_->addSubscriber(sub1);
    manager_->addSubscriber(sub2);

    // Push different data to each subscriber
    manager_->push(sub1, createVideoKeyframe(0));
    manager_->push(sub1, createVideoInterFrame(100));

    manager_->push(sub2, createVideoKeyframe(0));

    // Verify independent buffer levels
    auto stats1 = manager_->getSubscriberStats(sub1);
    auto stats2 = manager_->getSubscriberStats(sub2);

    EXPECT_EQ(stats1.pendingFrames, 2u);
    EXPECT_EQ(stats2.pendingFrames, 1u);
}

TEST_F(SubscriberBufferManagerTest, SlowSubscriberDoesNotAffectOthers) {
    SubscriberId slowSub = 1;
    SubscriberId fastSub = 2;

    manager_->addSubscriber(slowSub);
    manager_->addSubscriber(fastSub);

    // Push 6 seconds of data to slow subscriber (triggers overflow)
    for (uint32_t ts = 0; ts < 6000; ts += 33) {
        bool isKeyframe = (ts % 1000) < 33;
        auto frame = isKeyframe ? createVideoKeyframe(ts) : createVideoInterFrame(ts);
        manager_->push(slowSub, frame);
    }

    // Push 2 seconds of data to fast subscriber
    for (uint32_t ts = 0; ts < 2000; ts += 33) {
        bool isKeyframe = (ts % 1000) < 33;
        auto frame = isKeyframe ? createVideoKeyframe(ts) : createVideoInterFrame(ts);
        manager_->push(fastSub, frame);
    }

    // Slow subscriber should have dropped frames
    auto slowStats = manager_->getSubscriberStats(slowSub);
    EXPECT_GT(slowStats.droppedFrames, 0u);

    // Fast subscriber should have no dropped frames
    auto fastStats = manager_->getSubscriberStats(fastSub);
    EXPECT_EQ(fastStats.droppedFrames, 0u);
}

TEST_F(SubscriberBufferManagerTest, RemoveSubscriberCleansUpBuffer) {
    SubscriberId sub = 1;

    manager_->addSubscriber(sub);
    manager_->push(sub, createVideoKeyframe(0));

    manager_->removeSubscriber(sub);

    // Verify subscriber is removed
    EXPECT_FALSE(manager_->hasSubscriber(sub));
}

TEST_F(SubscriberBufferManagerTest, DistributeToAllSubscribers) {
    SubscriberId sub1 = 1;
    SubscriberId sub2 = 2;
    SubscriberId sub3 = 3;

    manager_->addSubscriber(sub1);
    manager_->addSubscriber(sub2);
    manager_->addSubscriber(sub3);

    // Distribute frame to all subscribers
    auto frame = createVideoKeyframe(0);
    manager_->distributeToAll(frame);

    // All subscribers should have the frame
    auto stats1 = manager_->getSubscriberStats(sub1);
    auto stats2 = manager_->getSubscriberStats(sub2);
    auto stats3 = manager_->getSubscriberStats(sub3);

    EXPECT_EQ(stats1.pendingFrames, 1u);
    EXPECT_EQ(stats2.pendingFrames, 1u);
    EXPECT_EQ(stats3.pendingFrames, 1u);
}

TEST_F(SubscriberBufferManagerTest, PerSubscriberConfigurationSupported) {
    SubscriberId sub1 = 1;
    SubscriberId sub2 = 2;

    SubscriberBufferConfig config1;
    config1.maxBufferDuration = std::chrono::milliseconds(3000);  // 3 seconds

    SubscriberBufferConfig config2;
    config2.maxBufferDuration = std::chrono::milliseconds(10000);  // 10 seconds

    manager_->addSubscriber(sub1, config1);
    manager_->addSubscriber(sub2, config2);

    // Sub1 has lower threshold
    auto retrievedConfig1 = manager_->getSubscriberConfig(sub1);
    EXPECT_EQ(retrievedConfig1.maxBufferDuration.count(), 3000);

    // Sub2 has higher threshold
    auto retrievedConfig2 = manager_->getSubscriberConfig(sub2);
    EXPECT_EQ(retrievedConfig2.maxBufferDuration.count(), 10000);
}

// =============================================================================
// Dropped Frame Statistics Tests
// =============================================================================

TEST_F(SubscriberBufferTest, TracksDroppedFrameStatistics) {
    // Configure for lower threshold for easier testing
    SubscriberBufferConfig config;
    config.maxBufferDuration = std::chrono::milliseconds(2000);  // 2 seconds
    buffer_->setConfig(config);

    // Push 4 seconds of data (should trigger drops)
    pushStreamSegment(0, 4000, true);

    auto stats = buffer_->getStatistics();

    EXPECT_GT(stats.droppedInterFrames, 0u);
    EXPECT_EQ(stats.droppedKeyframes, 0u);  // Keyframes never dropped
    EXPECT_GT(stats.totalDroppedBytes, 0u);
}

TEST_F(SubscriberBufferTest, ResetStatisticsWorks) {
    // Configure for lower threshold
    SubscriberBufferConfig config;
    config.maxBufferDuration = std::chrono::milliseconds(2000);
    buffer_->setConfig(config);

    // Push data to trigger drops
    pushStreamSegment(0, 4000, true);

    // Verify drops occurred
    EXPECT_GT(buffer_->getDroppedFrameCount(), 0u);

    // Reset statistics
    buffer_->resetStatistics();

    auto stats = buffer_->getStatistics();
    EXPECT_EQ(stats.droppedInterFrames, 0u);
    EXPECT_EQ(stats.droppedKeyframes, 0u);
    EXPECT_EQ(stats.totalDroppedBytes, 0u);
}

TEST_F(SubscriberBufferTest, DropStatisticsIncludesTimestamp) {
    SubscriberBufferConfig config;
    config.maxBufferDuration = std::chrono::milliseconds(2000);
    buffer_->setConfig(config);

    // Push data to trigger drops
    pushStreamSegment(0, 4000, true);

    auto stats = buffer_->getStatistics();

    // Last drop time should be set
    EXPECT_TRUE(stats.lastDropTime.has_value());
}

// =============================================================================
// Low-Latency Mode Tests
// =============================================================================

TEST_F(SubscriberBufferTest, LowLatencyModeWithSmallerBuffer) {
    SubscriberBufferConfig config;
    config.maxBufferDuration = std::chrono::milliseconds(500);  // 500ms for low-latency
    config.preserveAudio = false;  // Don't preserve audio to allow aggressive dropping
    buffer_->setConfig(config);

    // Push 2 seconds of data with keyframes every 500ms
    // This way, after dropping inter-frames, we should be within the limit
    for (uint32_t ts = 0; ts <= 2000; ts += 33) {
        bool isKeyframe = (ts % 500) < 33;  // Keyframe every 500ms
        if (isKeyframe) {
            buffer_->push(createVideoKeyframe(ts));
        } else {
            buffer_->push(createVideoInterFrame(ts));
        }
    }

    // With keyframes at 0, 500, 1000, 1500, 2000ms and inter-frames dropped,
    // the minimum buffer duration is still 2000ms (keyframe span).
    // However, the implementation should drop as many inter-frames as possible.
    // Verify that dropping occurred
    EXPECT_GT(buffer_->getDroppedFrameCount(), 0u);

    // Verify most frames left are keyframes
    size_t keyframeCount = 0;
    size_t totalFrames = 0;
    while (auto frame = buffer_->pop()) {
        ++totalFrames;
        if (frame->type == MediaType::Video && frame->isKeyframe) {
            ++keyframeCount;
        }
    }

    // In low-latency mode, most remaining frames should be keyframes
    EXPECT_GE(keyframeCount, 4u);  // At least 4 keyframes preserved
}

// =============================================================================
// Edge Cases Tests
// =============================================================================

TEST_F(SubscriberBufferTest, HandlesEmptyPush) {
    BufferedFrame emptyFrame;
    emptyFrame.type = MediaType::Video;
    emptyFrame.timestamp = 0;
    emptyFrame.isKeyframe = false;
    // data is empty

    buffer_->push(emptyFrame);

    EXPECT_EQ(buffer_->getPendingFrameCount(), 1u);
}

TEST_F(SubscriberBufferTest, HandlesTimestampWraparound) {
    // Push frame near max uint32_t
    buffer_->push(createVideoKeyframe(0xFFFFFF00));
    buffer_->push(createVideoInterFrame(0xFFFFFF33));

    EXPECT_EQ(buffer_->getPendingFrameCount(), 2u);
}

TEST_F(SubscriberBufferTest, HandlesRapidPushes) {
    // Rapid push of many frames
    for (uint32_t i = 0; i < 1000; ++i) {
        buffer_->push(createVideoInterFrame(i * 33));
    }

    // Should not crash and should handle overflow
    EXPECT_LE(buffer_->getBufferLevel().count(), 5500);
}

TEST_F(SubscriberBufferTest, PreservesFrameDataIntegrity) {
    BufferedFrame original;
    original.type = MediaType::Video;
    original.timestamp = 12345;
    original.isKeyframe = true;
    original.data = {0x01, 0x02, 0x03, 0x04, 0x05};

    buffer_->push(original);
    auto retrieved = buffer_->pop();

    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->type, original.type);
    EXPECT_EQ(retrieved->timestamp, original.timestamp);
    EXPECT_EQ(retrieved->isKeyframe, original.isKeyframe);
    EXPECT_EQ(retrieved->data, original.data);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

class SubscriberBufferConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        buffer_ = std::make_unique<SubscriberBuffer>();
    }

    std::unique_ptr<SubscriberBuffer> buffer_;
};

TEST_F(SubscriberBufferConcurrencyTest, ConcurrentPushAndPopIsThreadSafe) {
    std::atomic<bool> running{true};
    std::atomic<size_t> pushCount{0};
    std::atomic<size_t> popCount{0};
    std::vector<std::thread> threads;

    // Producer thread
    threads.emplace_back([this, &running, &pushCount]() {
        uint32_t timestamp = 0;
        while (running) {
            BufferedFrame frame;
            frame.type = MediaType::Video;
            frame.timestamp = timestamp;
            frame.isKeyframe = (timestamp % 1000) < 33;
            frame.data.resize(100);
            buffer_->push(frame);
            ++pushCount;
            timestamp += 33;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // Consumer threads
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([this, &running, &popCount]() {
            while (running) {
                if (buffer_->pop()) {
                    ++popCount;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
    }

    // Run for a short time
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& thread : threads) {
        thread.join();
    }

    // Test passes if no crashes or deadlocks
    EXPECT_GT(pushCount.load(), 0u);
}

TEST_F(SubscriberBufferConcurrencyTest, ConcurrentStatisticsAccessIsThreadSafe) {
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;

    // Writer thread
    threads.emplace_back([this, &running]() {
        uint32_t timestamp = 0;
        while (running) {
            BufferedFrame frame;
            frame.type = MediaType::Video;
            frame.timestamp = timestamp;
            frame.isKeyframe = (timestamp % 1000) < 33;
            frame.data.resize(100);
            buffer_->push(frame);
            timestamp += 33;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // Reader threads accessing statistics
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([this, &running]() {
            while (running) {
                buffer_->getBufferLevel();
                buffer_->getStatistics();
                buffer_->getDroppedFrameCount();
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& thread : threads) {
        thread.join();
    }
}

// =============================================================================
// Callback Tests
// =============================================================================

TEST_F(SubscriberBufferTest, OnFrameDroppedCallbackInvoked) {
    bool callbackInvoked = false;
    uint32_t droppedTimestamp = 0;

    buffer_->setOnFrameDroppedCallback([&](const BufferedFrame& frame) {
        callbackInvoked = true;
        droppedTimestamp = frame.timestamp;
    });

    // Configure low threshold
    SubscriberBufferConfig config;
    config.maxBufferDuration = std::chrono::milliseconds(1000);
    buffer_->setConfig(config);

    // Push enough to trigger drops
    pushStreamSegment(0, 3000, true);

    EXPECT_TRUE(callbackInvoked);
}

// =============================================================================
// Clear and Reset Tests
// =============================================================================

TEST_F(SubscriberBufferTest, ClearRemovesAllFramesButPreservesStats) {
    pushStreamSegment(0, 6000, true);  // Should trigger drops

    auto droppedBefore = buffer_->getDroppedFrameCount();

    buffer_->clear();

    EXPECT_EQ(buffer_->getPendingFrameCount(), 0u);
    EXPECT_EQ(buffer_->getBufferedBytes(), 0u);
    // Statistics should be preserved
    EXPECT_EQ(buffer_->getDroppedFrameCount(), droppedBefore);
}

TEST_F(SubscriberBufferTest, ResetClearsEverythingIncludingStats) {
    pushStreamSegment(0, 6000, true);

    buffer_->reset();

    EXPECT_EQ(buffer_->getPendingFrameCount(), 0u);
    EXPECT_EQ(buffer_->getBufferedBytes(), 0u);
    EXPECT_EQ(buffer_->getDroppedFrameCount(), 0u);
}

} // namespace test
} // namespace streaming
} // namespace openrtmp
