// OpenRTMP - Cross-platform RTMP Server
// Tests for Subscriber Manager
//
// Tests cover:
// - Add subscribers to active streams with configuration options
// - Maintain independent send buffers per subscriber
// - Support low-latency mode with 500ms maximum buffer
// - Remove subscribers cleanly on disconnect or stop
// - Track subscriber statistics including bytes delivered and dropped frames
//
// Requirements coverage:
// - Requirement 5.2: Support multiple simultaneous subscribers for a single stream
// - Requirement 5.4: Maintain independent send buffers for each subscriber
// - Requirement 12.4: Low-latency mode option reducing buffering
// - Requirement 12.5: Low-latency mode 500ms maximum buffer per subscriber

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <cstdint>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>

#include "openrtmp/streaming/subscriber_manager.hpp"
#include "openrtmp/streaming/subscriber_buffer.hpp"
#include "openrtmp/streaming/stream_registry.hpp"
#include "openrtmp/streaming/gop_buffer.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace streaming {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class SubscriberManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        streamRegistry_ = std::make_shared<StreamRegistry>();
        manager_ = std::make_unique<SubscriberManager>(streamRegistry_);

        // Register a test stream
        testStreamKey_ = StreamKey("live", "test_stream");
        auto streamIdResult = streamRegistry_->allocateStreamId();
        ASSERT_TRUE(streamIdResult.isSuccess());
        testStreamId_ = streamIdResult.value();

        auto result = streamRegistry_->registerStream(testStreamKey_, testStreamId_, 1);
        ASSERT_TRUE(result.isSuccess());
    }

    void TearDown() override {
        manager_.reset();
        streamRegistry_.reset();
    }

    std::shared_ptr<StreamRegistry> streamRegistry_;
    std::unique_ptr<SubscriberManager> manager_;
    StreamKey testStreamKey_;
    StreamId testStreamId_;

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
};

// =============================================================================
// Basic Subscriber Addition Tests (Requirement 5.2)
// =============================================================================

TEST_F(SubscriberManagerTest, AddSubscriberToActiveStream) {
    SubscriberId subscriberId = 100;

    auto result = manager_->addSubscriber(testStreamKey_, subscriberId);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(manager_->hasSubscriber(subscriberId));
}

TEST_F(SubscriberManagerTest, AddSubscriberFailsForNonExistentStream) {
    StreamKey invalidKey("live", "nonexistent_stream");
    SubscriberId subscriberId = 100;

    auto result = manager_->addSubscriber(invalidKey, subscriberId);

    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.error().code, SubscriberManagerError::Code::StreamNotFound);
}

TEST_F(SubscriberManagerTest, AddDuplicateSubscriberFails) {
    SubscriberId subscriberId = 100;

    auto result1 = manager_->addSubscriber(testStreamKey_, subscriberId);
    ASSERT_TRUE(result1.isSuccess());

    auto result2 = manager_->addSubscriber(testStreamKey_, subscriberId);
    EXPECT_FALSE(result2.isSuccess());
    EXPECT_EQ(result2.error().code, SubscriberManagerError::Code::SubscriberAlreadyExists);
}

TEST_F(SubscriberManagerTest, AddMultipleSubscribersToSameStream) {
    SubscriberId sub1 = 100;
    SubscriberId sub2 = 101;
    SubscriberId sub3 = 102;

    auto result1 = manager_->addSubscriber(testStreamKey_, sub1);
    auto result2 = manager_->addSubscriber(testStreamKey_, sub2);
    auto result3 = manager_->addSubscriber(testStreamKey_, sub3);

    EXPECT_TRUE(result1.isSuccess());
    EXPECT_TRUE(result2.isSuccess());
    EXPECT_TRUE(result3.isSuccess());

    EXPECT_EQ(manager_->getSubscriberCount(testStreamKey_), 3u);
}

// =============================================================================
// Subscriber Configuration Tests (Requirement 12.4)
// =============================================================================

TEST_F(SubscriberManagerTest, AddSubscriberWithCustomConfiguration) {
    SubscriberId subscriberId = 100;

    SubscriberConfig config;
    config.lowLatencyMode = true;
    config.maxBufferDuration = std::chrono::milliseconds(500);

    auto result = manager_->addSubscriber(testStreamKey_, subscriberId, config);

    EXPECT_TRUE(result.isSuccess());

    auto retrievedConfig = manager_->getSubscriberConfig(subscriberId);
    ASSERT_TRUE(retrievedConfig.has_value());
    EXPECT_TRUE(retrievedConfig->lowLatencyMode);
    EXPECT_EQ(retrievedConfig->maxBufferDuration.count(), 500);
}

TEST_F(SubscriberManagerTest, DefaultConfigurationIsNotLowLatency) {
    SubscriberId subscriberId = 100;

    auto result = manager_->addSubscriber(testStreamKey_, subscriberId);
    ASSERT_TRUE(result.isSuccess());

    auto config = manager_->getSubscriberConfig(subscriberId);
    ASSERT_TRUE(config.has_value());
    EXPECT_FALSE(config->lowLatencyMode);
    EXPECT_EQ(config->maxBufferDuration.count(), 5000);  // Default 5 seconds
}

// =============================================================================
// Low-Latency Mode Tests (Requirement 12.5)
// =============================================================================

TEST_F(SubscriberManagerTest, LowLatencyModeWith500msMaxBuffer) {
    SubscriberId subscriberId = 100;

    SubscriberConfig config;
    config.lowLatencyMode = true;
    config.maxBufferDuration = std::chrono::milliseconds(500);

    auto result = manager_->addSubscriber(testStreamKey_, subscriberId, config);
    ASSERT_TRUE(result.isSuccess());

    // Push frames to the subscriber buffer
    for (uint32_t ts = 0; ts <= 1000; ts += 33) {
        bool isKeyframe = (ts % 500) < 33;
        auto frame = isKeyframe ? createVideoKeyframe(ts) : createVideoInterFrame(ts);
        manager_->pushToSubscriber(subscriberId, frame);
    }

    // Low-latency mode should have triggered frame drops
    auto stats = manager_->getSubscriberStats(subscriberId);
    ASSERT_TRUE(stats.has_value());
    EXPECT_GT(stats->droppedFrames, 0u);
}

// =============================================================================
// Independent Buffer Tests (Requirement 5.4)
// =============================================================================

TEST_F(SubscriberManagerTest, MaintainsIndependentBuffersPerSubscriber) {
    SubscriberId sub1 = 100;
    SubscriberId sub2 = 101;

    manager_->addSubscriber(testStreamKey_, sub1);
    manager_->addSubscriber(testStreamKey_, sub2);

    // Push different amounts of data to each subscriber
    manager_->pushToSubscriber(sub1, createVideoKeyframe(0));
    manager_->pushToSubscriber(sub1, createVideoInterFrame(33));
    manager_->pushToSubscriber(sub1, createVideoInterFrame(66));

    manager_->pushToSubscriber(sub2, createVideoKeyframe(0));

    // Verify independent buffer levels
    auto stats1 = manager_->getSubscriberStats(sub1);
    auto stats2 = manager_->getSubscriberStats(sub2);

    ASSERT_TRUE(stats1.has_value());
    ASSERT_TRUE(stats2.has_value());

    EXPECT_NE(stats1->pendingFrames, stats2->pendingFrames);
}

TEST_F(SubscriberManagerTest, SlowSubscriberDoesNotAffectFastSubscriber) {
    SubscriberId slowSub = 100;
    SubscriberId fastSub = 101;

    // Use different configs - slow subscriber has lower threshold for easier testing
    SubscriberConfig slowConfig;
    slowConfig.maxBufferDuration = std::chrono::milliseconds(2000);

    SubscriberConfig fastConfig;
    fastConfig.maxBufferDuration = std::chrono::milliseconds(10000);

    manager_->addSubscriber(testStreamKey_, slowSub, slowConfig);
    manager_->addSubscriber(testStreamKey_, fastSub, fastConfig);

    // Push 4 seconds of data to slow subscriber (triggers overflow)
    for (uint32_t ts = 0; ts < 4000; ts += 33) {
        bool isKeyframe = (ts % 1000) < 33;
        auto frame = isKeyframe ? createVideoKeyframe(ts) : createVideoInterFrame(ts);
        manager_->pushToSubscriber(slowSub, frame);
    }

    // Push 2 seconds of data to fast subscriber
    for (uint32_t ts = 0; ts < 2000; ts += 33) {
        bool isKeyframe = (ts % 1000) < 33;
        auto frame = isKeyframe ? createVideoKeyframe(ts) : createVideoInterFrame(ts);
        manager_->pushToSubscriber(fastSub, frame);
    }

    // Slow subscriber should have dropped frames
    auto slowStats = manager_->getSubscriberStats(slowSub);
    ASSERT_TRUE(slowStats.has_value());
    EXPECT_GT(slowStats->droppedFrames, 0u);

    // Fast subscriber should have no dropped frames
    auto fastStats = manager_->getSubscriberStats(fastSub);
    ASSERT_TRUE(fastStats.has_value());
    EXPECT_EQ(fastStats->droppedFrames, 0u);
}

// =============================================================================
// Subscriber Removal Tests
// =============================================================================

TEST_F(SubscriberManagerTest, RemoveSubscriberCleanly) {
    SubscriberId subscriberId = 100;

    auto result = manager_->addSubscriber(testStreamKey_, subscriberId);
    ASSERT_TRUE(result.isSuccess());

    // Push some data
    manager_->pushToSubscriber(subscriberId, createVideoKeyframe(0));

    // Remove subscriber
    auto removeResult = manager_->removeSubscriber(subscriberId);
    EXPECT_TRUE(removeResult.isSuccess());
    EXPECT_FALSE(manager_->hasSubscriber(subscriberId));
}

TEST_F(SubscriberManagerTest, RemoveNonExistentSubscriberFails) {
    SubscriberId nonExistentId = 999;

    auto result = manager_->removeSubscriber(nonExistentId);
    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.error().code, SubscriberManagerError::Code::SubscriberNotFound);
}

TEST_F(SubscriberManagerTest, RemoveSubscriberByDisconnect) {
    SubscriberId subscriberId = 100;

    manager_->addSubscriber(testStreamKey_, subscriberId);

    // Simulate disconnect
    manager_->onSubscriberDisconnect(subscriberId);

    EXPECT_FALSE(manager_->hasSubscriber(subscriberId));
}

TEST_F(SubscriberManagerTest, RemoveAllSubscribersOnStreamEnd) {
    SubscriberId sub1 = 100;
    SubscriberId sub2 = 101;
    SubscriberId sub3 = 102;

    manager_->addSubscriber(testStreamKey_, sub1);
    manager_->addSubscriber(testStreamKey_, sub2);
    manager_->addSubscriber(testStreamKey_, sub3);

    // Simulate stream end
    manager_->onStreamEnded(testStreamKey_);

    EXPECT_EQ(manager_->getSubscriberCount(testStreamKey_), 0u);
}

// =============================================================================
// Subscriber Statistics Tests
// =============================================================================

TEST_F(SubscriberManagerTest, TracksBytesDelivered) {
    SubscriberId subscriberId = 100;

    manager_->addSubscriber(testStreamKey_, subscriberId);

    // Push frames
    manager_->pushToSubscriber(subscriberId, createVideoKeyframe(0, 1000));
    manager_->pushToSubscriber(subscriberId, createVideoInterFrame(33, 500));

    // Simulate delivery by popping frames
    manager_->popFromSubscriber(subscriberId);
    manager_->popFromSubscriber(subscriberId);

    auto stats = manager_->getSubscriberStats(subscriberId);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->bytesDelivered, 1500u);
}

TEST_F(SubscriberManagerTest, TracksDroppedFrames) {
    SubscriberId subscriberId = 100;

    // Configure low buffer threshold
    SubscriberConfig config;
    config.maxBufferDuration = std::chrono::milliseconds(1000);

    manager_->addSubscriber(testStreamKey_, subscriberId, config);

    // Push 3 seconds of data to trigger drops
    for (uint32_t ts = 0; ts < 3000; ts += 33) {
        bool isKeyframe = (ts % 1000) < 33;
        auto frame = isKeyframe ? createVideoKeyframe(ts) : createVideoInterFrame(ts);
        manager_->pushToSubscriber(subscriberId, frame);
    }

    auto stats = manager_->getSubscriberStats(subscriberId);
    ASSERT_TRUE(stats.has_value());
    EXPECT_GT(stats->droppedFrames, 0u);
}

TEST_F(SubscriberManagerTest, TracksTotalStatisticsPerSubscriber) {
    SubscriberId subscriberId = 100;

    manager_->addSubscriber(testStreamKey_, subscriberId);

    // Push and pop some frames
    manager_->pushToSubscriber(subscriberId, createVideoKeyframe(0, 1000));
    manager_->pushToSubscriber(subscriberId, createVideoInterFrame(33, 500));
    manager_->popFromSubscriber(subscriberId);

    auto stats = manager_->getSubscriberStats(subscriberId);
    ASSERT_TRUE(stats.has_value());

    EXPECT_EQ(stats->subscriberId, subscriberId);
    EXPECT_EQ(stats->pendingFrames, 1u);
    EXPECT_GT(stats->pendingBytes, 0u);
}

TEST_F(SubscriberManagerTest, GetAllSubscriberStats) {
    SubscriberId sub1 = 100;
    SubscriberId sub2 = 101;

    manager_->addSubscriber(testStreamKey_, sub1);
    manager_->addSubscriber(testStreamKey_, sub2);

    manager_->pushToSubscriber(sub1, createVideoKeyframe(0));
    manager_->pushToSubscriber(sub2, createVideoKeyframe(0));
    manager_->pushToSubscriber(sub2, createVideoInterFrame(33));

    auto allStats = manager_->getAllSubscriberStats(testStreamKey_);

    EXPECT_EQ(allStats.size(), 2u);
}

// =============================================================================
// Stream Lookup Integration Tests
// =============================================================================

TEST_F(SubscriberManagerTest, FindsStreamFromRegistry) {
    SubscriberId subscriberId = 100;

    auto result = manager_->addSubscriber(testStreamKey_, subscriberId);
    ASSERT_TRUE(result.isSuccess());

    auto streamKey = manager_->getSubscriberStreamKey(subscriberId);
    ASSERT_TRUE(streamKey.has_value());
    EXPECT_EQ(*streamKey, testStreamKey_);
}

TEST_F(SubscriberManagerTest, GetSubscribersForStream) {
    SubscriberId sub1 = 100;
    SubscriberId sub2 = 101;

    manager_->addSubscriber(testStreamKey_, sub1);
    manager_->addSubscriber(testStreamKey_, sub2);

    auto subscribers = manager_->getSubscribersForStream(testStreamKey_);

    EXPECT_EQ(subscribers.size(), 2u);
    EXPECT_TRUE(subscribers.find(sub1) != subscribers.end());
    EXPECT_TRUE(subscribers.find(sub2) != subscribers.end());
}

// =============================================================================
// Buffer Level Tests
// =============================================================================

TEST_F(SubscriberManagerTest, GetSubscriberBufferLevel) {
    SubscriberId subscriberId = 100;

    manager_->addSubscriber(testStreamKey_, subscriberId);

    manager_->pushToSubscriber(subscriberId, createVideoKeyframe(0));
    manager_->pushToSubscriber(subscriberId, createVideoInterFrame(1000));  // 1 second later

    auto bufferLevel = manager_->getSubscriberBufferLevel(subscriberId);
    ASSERT_TRUE(bufferLevel.has_value());
    EXPECT_GE(bufferLevel->count(), 1000);  // At least 1 second
}

// =============================================================================
// Distribute to All Subscribers Test
// =============================================================================

TEST_F(SubscriberManagerTest, DistributeFrameToAllSubscribers) {
    SubscriberId sub1 = 100;
    SubscriberId sub2 = 101;
    SubscriberId sub3 = 102;

    manager_->addSubscriber(testStreamKey_, sub1);
    manager_->addSubscriber(testStreamKey_, sub2);
    manager_->addSubscriber(testStreamKey_, sub3);

    // Distribute a frame to all subscribers of the stream
    auto frame = createVideoKeyframe(0);
    manager_->distributeToStream(testStreamKey_, frame);

    // All subscribers should have the frame
    auto stats1 = manager_->getSubscriberStats(sub1);
    auto stats2 = manager_->getSubscriberStats(sub2);
    auto stats3 = manager_->getSubscriberStats(sub3);

    ASSERT_TRUE(stats1.has_value());
    ASSERT_TRUE(stats2.has_value());
    ASSERT_TRUE(stats3.has_value());

    EXPECT_EQ(stats1->pendingFrames, 1u);
    EXPECT_EQ(stats2->pendingFrames, 1u);
    EXPECT_EQ(stats3->pendingFrames, 1u);
}

// =============================================================================
// Lifecycle Tests
// =============================================================================

TEST_F(SubscriberManagerTest, ClearAllSubscribers) {
    SubscriberId sub1 = 100;
    SubscriberId sub2 = 101;

    manager_->addSubscriber(testStreamKey_, sub1);
    manager_->addSubscriber(testStreamKey_, sub2);

    manager_->clear();

    EXPECT_EQ(manager_->getTotalSubscriberCount(), 0u);
}

TEST_F(SubscriberManagerTest, GetTotalSubscriberCount) {
    // Register another stream
    StreamKey otherKey("live", "other_stream");
    auto streamIdResult = streamRegistry_->allocateStreamId();
    ASSERT_TRUE(streamIdResult.isSuccess());
    streamRegistry_->registerStream(otherKey, streamIdResult.value(), 2);

    // Add subscribers to both streams
    manager_->addSubscriber(testStreamKey_, 100);
    manager_->addSubscriber(testStreamKey_, 101);
    manager_->addSubscriber(otherKey, 200);

    EXPECT_EQ(manager_->getTotalSubscriberCount(), 3u);
}

// =============================================================================
// Callback Tests
// =============================================================================

TEST_F(SubscriberManagerTest, OnSubscriberAddedCallbackInvoked) {
    bool callbackInvoked = false;
    SubscriberId receivedSubId = 0;
    StreamKey receivedStreamKey;

    manager_->setOnSubscriberAddedCallback(
        [&](SubscriberId subId, const StreamKey& key) {
            callbackInvoked = true;
            receivedSubId = subId;
            receivedStreamKey = key;
        }
    );

    SubscriberId subscriberId = 100;
    manager_->addSubscriber(testStreamKey_, subscriberId);

    EXPECT_TRUE(callbackInvoked);
    EXPECT_EQ(receivedSubId, subscriberId);
    EXPECT_EQ(receivedStreamKey, testStreamKey_);
}

TEST_F(SubscriberManagerTest, OnSubscriberRemovedCallbackInvoked) {
    bool callbackInvoked = false;
    SubscriberId receivedSubId = 0;

    manager_->setOnSubscriberRemovedCallback(
        [&](SubscriberId subId, const StreamKey& /*key*/) {
            callbackInvoked = true;
            receivedSubId = subId;
        }
    );

    SubscriberId subscriberId = 100;
    manager_->addSubscriber(testStreamKey_, subscriberId);
    manager_->removeSubscriber(subscriberId);

    EXPECT_TRUE(callbackInvoked);
    EXPECT_EQ(receivedSubId, subscriberId);
}

TEST_F(SubscriberManagerTest, OnFrameDroppedCallbackInvoked) {
    bool callbackInvoked = false;

    manager_->setOnFrameDroppedCallback(
        [&](SubscriberId /*subId*/, const BufferedFrame& /*frame*/) {
            callbackInvoked = true;
        }
    );

    // Configure low threshold to trigger drops
    SubscriberConfig config;
    config.maxBufferDuration = std::chrono::milliseconds(1000);

    SubscriberId subscriberId = 100;
    manager_->addSubscriber(testStreamKey_, subscriberId, config);

    // Push enough to trigger drops
    for (uint32_t ts = 0; ts < 3000; ts += 33) {
        bool isKeyframe = (ts % 1000) < 33;
        auto frame = isKeyframe ? createVideoKeyframe(ts) : createVideoInterFrame(ts);
        manager_->pushToSubscriber(subscriberId, frame);
    }

    EXPECT_TRUE(callbackInvoked);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

class SubscriberManagerConcurrencyTest : public SubscriberManagerTest {};

TEST_F(SubscriberManagerConcurrencyTest, ConcurrentSubscriberOperationsAreThreadSafe) {
    std::atomic<bool> running{true};
    std::atomic<size_t> addCount{0};
    std::atomic<size_t> removeCount{0};
    std::vector<std::thread> threads;

    // Producer threads - add subscribers
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([this, i, &running, &addCount]() {
            SubscriberId subId = static_cast<SubscriberId>(1000 + i * 100);
            while (running) {
                if (manager_->addSubscriber(testStreamKey_, subId).isSuccess()) {
                    ++addCount;
                    subId += 3;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }

    // Consumer threads - remove subscribers
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([this, i, &running, &removeCount]() {
            SubscriberId subId = static_cast<SubscriberId>(1000 + i);
            while (running) {
                if (manager_->removeSubscriber(subId).isSuccess()) {
                    ++removeCount;
                }
                subId += 2;
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
    EXPECT_GT(addCount.load(), 0u);
}

TEST_F(SubscriberManagerConcurrencyTest, ConcurrentPushAndStatisticsAreThreadSafe) {
    SubscriberId subscriberId = 100;
    manager_->addSubscriber(testStreamKey_, subscriberId);

    std::atomic<bool> running{true};
    std::vector<std::thread> threads;

    // Writer thread
    threads.emplace_back([this, subscriberId, &running]() {
        uint32_t timestamp = 0;
        while (running) {
            auto frame = createVideoInterFrame(timestamp);
            manager_->pushToSubscriber(subscriberId, frame);
            timestamp += 33;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // Reader threads
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([this, subscriberId, &running]() {
            while (running) {
                manager_->getSubscriberStats(subscriberId);
                manager_->getSubscriberBufferLevel(subscriberId);
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
// Edge Cases Tests
// =============================================================================

TEST_F(SubscriberManagerTest, HandleZeroSubscriberId) {
    SubscriberId zeroId = 0;  // Invalid ID

    auto result = manager_->addSubscriber(testStreamKey_, zeroId);

    // Zero is typically used as invalid, but implementation may allow it
    // Check that at least it doesn't crash
    if (result.isSuccess()) {
        EXPECT_TRUE(manager_->hasSubscriber(zeroId));
    }
}

TEST_F(SubscriberManagerTest, GetStatsForNonExistentSubscriber) {
    auto stats = manager_->getSubscriberStats(999);
    EXPECT_FALSE(stats.has_value());
}

TEST_F(SubscriberManagerTest, PopFromEmptySubscriberBuffer) {
    SubscriberId subscriberId = 100;
    manager_->addSubscriber(testStreamKey_, subscriberId);

    auto frame = manager_->popFromSubscriber(subscriberId);
    EXPECT_FALSE(frame.has_value());
}

TEST_F(SubscriberManagerTest, GetConfigForNonExistentSubscriber) {
    auto config = manager_->getSubscriberConfig(999);
    EXPECT_FALSE(config.has_value());
}

} // namespace test
} // namespace streaming
} // namespace openrtmp
