// OpenRTMP - Cross-platform RTMP Server
// Tests for Media Distribution Engine
//
// Tests cover:
// - Send cached metadata and sequence headers on subscription start
// - Transmit from most recent keyframe for instant playback
// - Forward live media from ingestion pipeline to all subscribers
// - Send stream EOF message within 1 second of stream end
// - Implement slow subscriber detection and frame dropping
//
// Requirements coverage:
// - Requirement 5.1: Transmit data starting from most recent keyframe
// - Requirement 5.3: Send cached metadata and sequence headers before stream data
// - Requirement 5.6: Send stream EOF message within 1 second of stream end

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <cstdint>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>

#include "openrtmp/streaming/media_distribution.hpp"
#include "openrtmp/streaming/subscriber_manager.hpp"
#include "openrtmp/streaming/stream_registry.hpp"
#include "openrtmp/streaming/gop_buffer.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace streaming {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class MediaDistributionTest : public ::testing::Test {
protected:
    void SetUp() override {
        streamRegistry_ = std::make_shared<StreamRegistry>();
        subscriberManager_ = std::make_shared<SubscriberManager>(streamRegistry_);
        distribution_ = std::make_unique<MediaDistribution>(
            streamRegistry_,
            subscriberManager_
        );

        // Register a test stream
        testStreamKey_ = StreamKey("live", "test_stream");
        auto streamIdResult = streamRegistry_->allocateStreamId();
        ASSERT_TRUE(streamIdResult.isSuccess());
        testStreamId_ = streamIdResult.value();

        auto result = streamRegistry_->registerStream(testStreamKey_, testStreamId_, 1);
        ASSERT_TRUE(result.isSuccess());

        // Create GOP buffer for the stream
        gopBuffer_ = std::make_shared<GOPBuffer>();
        distribution_->setGOPBuffer(testStreamKey_, gopBuffer_);
    }

    void TearDown() override {
        distribution_.reset();
        subscriberManager_.reset();
        streamRegistry_.reset();
    }

    std::shared_ptr<StreamRegistry> streamRegistry_;
    std::shared_ptr<SubscriberManager> subscriberManager_;
    std::unique_ptr<MediaDistribution> distribution_;
    std::shared_ptr<GOPBuffer> gopBuffer_;
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
        for (size_t i = 0; i < dataSize; ++i) {
            frame.data[i] = static_cast<uint8_t>((timestamp + i) & 0xFF);
        }
        return frame;
    }

    // Helper to create a video keyframe
    BufferedFrame createVideoKeyframe(uint32_t timestamp, size_t dataSize = 1000) {
        return createFrame(MediaType::Video, timestamp, true, dataSize);
    }

    // Helper to create a video inter frame
    BufferedFrame createVideoInterFrame(uint32_t timestamp, size_t dataSize = 500) {
        return createFrame(MediaType::Video, timestamp, false, dataSize);
    }

    // Helper to create an audio frame
    BufferedFrame createAudioFrame(uint32_t timestamp, size_t dataSize = 200) {
        return createFrame(MediaType::Audio, timestamp, true, dataSize);
    }

    // Helper to create test metadata
    protocol::AMFValue createTestMetadata() {
        std::map<std::string, protocol::AMFValue> obj;
        obj["width"] = protocol::AMFValue::makeNumber(1920);
        obj["height"] = protocol::AMFValue::makeNumber(1080);
        obj["framerate"] = protocol::AMFValue::makeNumber(30);
        return protocol::AMFValue::makeObject(std::move(obj));
    }

    // Helper to create test sequence headers
    std::vector<uint8_t> createVideoSeqHeader() {
        return {0x17, 0x00, 0x00, 0x00, 0x00, 0x01, 0x64, 0x00, 0x1f};
    }

    std::vector<uint8_t> createAudioSeqHeader() {
        return {0xAF, 0x00, 0x11, 0x90};
    }
};

// =============================================================================
// Subscription Start Tests (Requirement 5.3)
// =============================================================================

TEST_F(MediaDistributionTest, SendsMetadataOnSubscriptionStart) {
    // Setup: Store metadata in GOP buffer
    gopBuffer_->setMetadata(createTestMetadata());

    // Track what gets sent to subscriber
    std::vector<BufferedFrame> sentFrames;
    distribution_->setOnFrameSentCallback(
        [&](SubscriberId, const BufferedFrame& frame) {
            sentFrames.push_back(frame);
        }
    );

    // Add subscriber
    SubscriberId subscriberId = 100;
    auto result = distribution_->addSubscriber(testStreamKey_, subscriberId);
    ASSERT_TRUE(result.isSuccess());

    // Verify metadata was sent first
    ASSERT_GE(sentFrames.size(), 1u);
    EXPECT_EQ(sentFrames[0].type, MediaType::Data);
}

TEST_F(MediaDistributionTest, SendsSequenceHeadersOnSubscriptionStart) {
    // Setup: Store sequence headers in GOP buffer
    gopBuffer_->setSequenceHeaders(createVideoSeqHeader(), createAudioSeqHeader());

    std::vector<BufferedFrame> sentFrames;
    distribution_->setOnFrameSentCallback(
        [&](SubscriberId, const BufferedFrame& frame) {
            sentFrames.push_back(frame);
        }
    );

    SubscriberId subscriberId = 100;
    auto result = distribution_->addSubscriber(testStreamKey_, subscriberId);
    ASSERT_TRUE(result.isSuccess());

    // Verify sequence headers were sent
    bool hasVideoHeader = false;
    bool hasAudioHeader = false;
    for (const auto& frame : sentFrames) {
        if (frame.type == MediaType::Video && frame.isKeyframe) {
            hasVideoHeader = true;
        }
        if (frame.type == MediaType::Audio) {
            hasAudioHeader = true;
        }
    }
    EXPECT_TRUE(hasVideoHeader);
    EXPECT_TRUE(hasAudioHeader);
}

TEST_F(MediaDistributionTest, SendsMetadataBeforeSequenceHeaders) {
    gopBuffer_->setMetadata(createTestMetadata());
    gopBuffer_->setSequenceHeaders(createVideoSeqHeader(), createAudioSeqHeader());

    std::vector<MediaType> sendOrder;
    distribution_->setOnFrameSentCallback(
        [&](SubscriberId, const BufferedFrame& frame) {
            sendOrder.push_back(frame.type);
        }
    );

    SubscriberId subscriberId = 100;
    distribution_->addSubscriber(testStreamKey_, subscriberId);

    // First frame should be Data (metadata)
    ASSERT_GE(sendOrder.size(), 1u);
    EXPECT_EQ(sendOrder[0], MediaType::Data);
}

// =============================================================================
// Instant Playback Tests (Requirement 5.1)
// =============================================================================

TEST_F(MediaDistributionTest, TransmitsFromMostRecentKeyframe) {
    // Setup: Add frames to GOP buffer
    gopBuffer_->push(createVideoKeyframe(0));      // Old keyframe
    gopBuffer_->push(createVideoInterFrame(33));
    gopBuffer_->push(createVideoInterFrame(66));
    gopBuffer_->push(createVideoKeyframe(1000));   // Most recent keyframe
    gopBuffer_->push(createVideoInterFrame(1033));

    std::vector<BufferedFrame> sentFrames;
    distribution_->setOnFrameSentCallback(
        [&](SubscriberId, const BufferedFrame& frame) {
            sentFrames.push_back(frame);
        }
    );

    SubscriberId subscriberId = 100;
    distribution_->addSubscriber(testStreamKey_, subscriberId);

    // Should start from the most recent keyframe (timestamp 1000)
    bool foundKeyframe = false;
    for (const auto& frame : sentFrames) {
        if (frame.type == MediaType::Video && frame.isKeyframe && frame.timestamp == 1000) {
            foundKeyframe = true;
            break;
        }
    }
    EXPECT_TRUE(foundKeyframe);
}

TEST_F(MediaDistributionTest, DoesNotSendFramesBeforeMostRecentKeyframe) {
    gopBuffer_->push(createVideoKeyframe(0));      // Old keyframe - should NOT be sent
    gopBuffer_->push(createVideoInterFrame(33));   // Old frame - should NOT be sent
    gopBuffer_->push(createVideoKeyframe(1000));   // Most recent keyframe - SHOULD be sent
    gopBuffer_->push(createVideoInterFrame(1033)); // After keyframe - SHOULD be sent

    std::vector<BufferedFrame> sentFrames;
    distribution_->setOnFrameSentCallback(
        [&](SubscriberId, const BufferedFrame& frame) {
            if (frame.type == MediaType::Video) {
                sentFrames.push_back(frame);
            }
        }
    );

    SubscriberId subscriberId = 100;
    distribution_->addSubscriber(testStreamKey_, subscriberId);

    // Should not have any frames with timestamp < 1000
    for (const auto& frame : sentFrames) {
        EXPECT_GE(frame.timestamp, 1000u);
    }
}

// =============================================================================
// Live Media Distribution Tests
// =============================================================================

TEST_F(MediaDistributionTest, ForwardsLiveMediaToAllSubscribers) {
    // Add multiple subscribers
    SubscriberId sub1 = 100;
    SubscriberId sub2 = 101;
    SubscriberId sub3 = 102;

    distribution_->addSubscriber(testStreamKey_, sub1);
    distribution_->addSubscriber(testStreamKey_, sub2);
    distribution_->addSubscriber(testStreamKey_, sub3);

    std::map<SubscriberId, int> frameCountBySubscriber;
    distribution_->setOnFrameSentCallback(
        [&](SubscriberId subId, const BufferedFrame&) {
            frameCountBySubscriber[subId]++;
        }
    );

    // Distribute live media
    auto liveFrame = createVideoKeyframe(2000);
    distribution_->distribute(testStreamKey_, liveFrame);

    // All subscribers should receive the frame
    EXPECT_GE(frameCountBySubscriber[sub1], 1);
    EXPECT_GE(frameCountBySubscriber[sub2], 1);
    EXPECT_GE(frameCountBySubscriber[sub3], 1);
}

TEST_F(MediaDistributionTest, ForwardsAudioAndVideoFrames) {
    SubscriberId subscriberId = 100;
    distribution_->addSubscriber(testStreamKey_, subscriberId);

    std::vector<MediaType> receivedTypes;
    distribution_->setOnFrameSentCallback(
        [&](SubscriberId, const BufferedFrame& frame) {
            receivedTypes.push_back(frame.type);
        }
    );

    // Distribute different media types
    distribution_->distribute(testStreamKey_, createVideoKeyframe(0));
    distribution_->distribute(testStreamKey_, createAudioFrame(10));
    distribution_->distribute(testStreamKey_, createVideoInterFrame(33));
    distribution_->distribute(testStreamKey_, createAudioFrame(43));

    // Should have both audio and video
    bool hasVideo = std::find(receivedTypes.begin(), receivedTypes.end(), MediaType::Video) != receivedTypes.end();
    bool hasAudio = std::find(receivedTypes.begin(), receivedTypes.end(), MediaType::Audio) != receivedTypes.end();
    EXPECT_TRUE(hasVideo);
    EXPECT_TRUE(hasAudio);
}

// =============================================================================
// Stream End Tests (Requirement 5.6)
// =============================================================================

TEST_F(MediaDistributionTest, SendsEOFMessageOnStreamEnd) {
    SubscriberId subscriberId = 100;
    distribution_->addSubscriber(testStreamKey_, subscriberId);

    bool eofReceived = false;
    distribution_->setOnStreamEOFCallback(
        [&](SubscriberId subId, const StreamKey& key) {
            if (subId == subscriberId && key == testStreamKey_) {
                eofReceived = true;
            }
        }
    );

    // End the stream
    distribution_->onStreamEnd(testStreamKey_);

    EXPECT_TRUE(eofReceived);
}

TEST_F(MediaDistributionTest, SendsEOFToAllSubscribers) {
    SubscriberId sub1 = 100;
    SubscriberId sub2 = 101;
    SubscriberId sub3 = 102;

    distribution_->addSubscriber(testStreamKey_, sub1);
    distribution_->addSubscriber(testStreamKey_, sub2);
    distribution_->addSubscriber(testStreamKey_, sub3);

    std::set<SubscriberId> eofReceivers;
    distribution_->setOnStreamEOFCallback(
        [&](SubscriberId subId, const StreamKey&) {
            eofReceivers.insert(subId);
        }
    );

    distribution_->onStreamEnd(testStreamKey_);

    // All subscribers should receive EOF
    EXPECT_TRUE(eofReceivers.count(sub1) > 0);
    EXPECT_TRUE(eofReceivers.count(sub2) > 0);
    EXPECT_TRUE(eofReceivers.count(sub3) > 0);
}

TEST_F(MediaDistributionTest, EOFSentWithinOneSecond) {
    SubscriberId subscriberId = 100;
    distribution_->addSubscriber(testStreamKey_, subscriberId);

    std::chrono::steady_clock::time_point eofTime;
    distribution_->setOnStreamEOFCallback(
        [&](SubscriberId, const StreamKey&) {
            eofTime = std::chrono::steady_clock::now();
        }
    );

    auto startTime = std::chrono::steady_clock::now();
    distribution_->onStreamEnd(testStreamKey_);

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(eofTime - startTime);
    EXPECT_LT(duration.count(), 1000);  // Less than 1 second
}

// =============================================================================
// Slow Subscriber Detection Tests
// =============================================================================

TEST_F(MediaDistributionTest, DetectsSlowSubscriber) {
    SubscriberConfig slowConfig;
    slowConfig.maxBufferDuration = std::chrono::milliseconds(1000);

    SubscriberId subscriberId = 100;
    distribution_->addSubscriber(testStreamKey_, subscriberId, slowConfig);

    bool slowSubscriberDetected = false;
    distribution_->setOnSlowSubscriberCallback(
        [&](SubscriberId subId) {
            if (subId == subscriberId) {
                slowSubscriberDetected = true;
            }
        }
    );

    // Flood with data to trigger slow subscriber detection
    for (uint32_t ts = 0; ts < 3000; ts += 33) {
        bool isKeyframe = (ts % 1000) < 33;
        auto frame = isKeyframe ? createVideoKeyframe(ts) : createVideoInterFrame(ts);
        distribution_->distribute(testStreamKey_, frame);
    }

    EXPECT_TRUE(slowSubscriberDetected);
}

TEST_F(MediaDistributionTest, DropsFramesForSlowSubscriber) {
    SubscriberConfig slowConfig;
    slowConfig.maxBufferDuration = std::chrono::milliseconds(1000);

    SubscriberId subscriberId = 100;
    distribution_->addSubscriber(testStreamKey_, subscriberId, slowConfig);

    // Distribute 3 seconds of data
    for (uint32_t ts = 0; ts < 3000; ts += 33) {
        bool isKeyframe = (ts % 1000) < 33;
        auto frame = isKeyframe ? createVideoKeyframe(ts) : createVideoInterFrame(ts);
        distribution_->distribute(testStreamKey_, frame);
    }

    // Check subscriber stats - should have dropped frames
    auto stats = subscriberManager_->getSubscriberStats(subscriberId);
    ASSERT_TRUE(stats.has_value());
    EXPECT_GT(stats->droppedFrames, 0u);
}

// =============================================================================
// Subscriber Lifecycle Tests
// =============================================================================

TEST_F(MediaDistributionTest, AddSubscriberWithConfig) {
    SubscriberId subscriberId = 100;
    SubscriberConfig config;
    config.lowLatencyMode = true;
    config.maxBufferDuration = std::chrono::milliseconds(500);

    auto result = distribution_->addSubscriber(testStreamKey_, subscriberId, config);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(distribution_->hasSubscriber(subscriberId));
}

TEST_F(MediaDistributionTest, RemoveSubscriber) {
    SubscriberId subscriberId = 100;
    distribution_->addSubscriber(testStreamKey_, subscriberId);
    ASSERT_TRUE(distribution_->hasSubscriber(subscriberId));

    auto result = distribution_->removeSubscriber(subscriberId);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_FALSE(distribution_->hasSubscriber(subscriberId));
}

TEST_F(MediaDistributionTest, DoesNotDistributeToRemovedSubscriber) {
    SubscriberId subscriberId = 100;
    distribution_->addSubscriber(testStreamKey_, subscriberId);
    distribution_->removeSubscriber(subscriberId);

    int framesSent = 0;
    distribution_->setOnFrameSentCallback(
        [&](SubscriberId subId, const BufferedFrame&) {
            if (subId == subscriberId) {
                framesSent++;
            }
        }
    );

    distribution_->distribute(testStreamKey_, createVideoKeyframe(0));

    EXPECT_EQ(framesSent, 0);
}

// =============================================================================
// GOP Buffer Integration Tests
// =============================================================================

TEST_F(MediaDistributionTest, UpdatesGOPBufferOnDistribute) {
    // GOP buffer should be updated when distributing new frames
    EXPECT_EQ(gopBuffer_->getFrameCount(), 0u);

    distribution_->distribute(testStreamKey_, createVideoKeyframe(0));
    distribution_->distribute(testStreamKey_, createVideoInterFrame(33));

    // GOP buffer should have frames
    EXPECT_GE(gopBuffer_->getFrameCount(), 2u);
}

TEST_F(MediaDistributionTest, CachesSequenceHeaders) {
    std::vector<uint8_t> videoHeader = createVideoSeqHeader();
    std::vector<uint8_t> audioHeader = createAudioSeqHeader();

    distribution_->setSequenceHeaders(testStreamKey_, videoHeader, audioHeader);

    auto headers = gopBuffer_->getSequenceHeaders();
    EXPECT_EQ(headers.videoHeader, videoHeader);
    EXPECT_EQ(headers.audioHeader, audioHeader);
}

TEST_F(MediaDistributionTest, CachesMetadata) {
    auto metadata = createTestMetadata();
    distribution_->setMetadata(testStreamKey_, metadata);

    auto cached = gopBuffer_->getMetadata();
    ASSERT_TRUE(cached.has_value());
}

// =============================================================================
// Statistics Tests
// =============================================================================

TEST_F(MediaDistributionTest, TracksDistributionStatistics) {
    SubscriberId subscriberId = 100;
    distribution_->addSubscriber(testStreamKey_, subscriberId);

    // Distribute some frames
    distribution_->distribute(testStreamKey_, createVideoKeyframe(0));
    distribution_->distribute(testStreamKey_, createVideoInterFrame(33));
    distribution_->distribute(testStreamKey_, createAudioFrame(10));

    auto stats = distribution_->getDistributionStats(testStreamKey_);
    ASSERT_TRUE(stats.has_value());
    EXPECT_GT(stats->totalFramesDistributed, 0u);
    EXPECT_GT(stats->totalBytesDistributed, 0u);
}

TEST_F(MediaDistributionTest, TracksSubscriberCount) {
    distribution_->addSubscriber(testStreamKey_, 100);
    distribution_->addSubscriber(testStreamKey_, 101);
    distribution_->addSubscriber(testStreamKey_, 102);

    EXPECT_EQ(distribution_->getSubscriberCount(testStreamKey_), 3u);

    distribution_->removeSubscriber(100);
    EXPECT_EQ(distribution_->getSubscriberCount(testStreamKey_), 2u);
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST_F(MediaDistributionTest, AddSubscriberFailsForNonExistentStream) {
    StreamKey invalidKey("live", "nonexistent");
    SubscriberId subscriberId = 100;

    auto result = distribution_->addSubscriber(invalidKey, subscriberId);

    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.error().code, MediaDistributionError::Code::StreamNotFound);
}

TEST_F(MediaDistributionTest, DistributeToNonExistentStreamDoesNotCrash) {
    StreamKey invalidKey("live", "nonexistent");
    auto frame = createVideoKeyframe(0);

    // Should not crash, just silently ignore
    EXPECT_NO_THROW(distribution_->distribute(invalidKey, frame));
}

TEST_F(MediaDistributionTest, HandleNullGOPBuffer) {
    StreamKey newStreamKey("live", "no_gop");
    auto streamIdResult = streamRegistry_->allocateStreamId();
    ASSERT_TRUE(streamIdResult.isSuccess());
    streamRegistry_->registerStream(newStreamKey, streamIdResult.value(), 2);

    // Don't set GOP buffer for this stream
    SubscriberId subscriberId = 100;
    auto result = distribution_->addSubscriber(newStreamKey, subscriberId);

    // Should succeed but not send any cached data
    EXPECT_TRUE(result.isSuccess());
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

class MediaDistributionConcurrencyTest : public MediaDistributionTest {};

TEST_F(MediaDistributionConcurrencyTest, ConcurrentDistributionIsThreadSafe) {
    // Add multiple subscribers
    for (SubscriberId id = 100; id < 110; ++id) {
        distribution_->addSubscriber(testStreamKey_, id);
    }

    std::atomic<bool> running{true};
    std::vector<std::thread> threads;

    // Multiple producer threads
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([this, &running, i]() {
            uint32_t timestamp = static_cast<uint32_t>(i * 10000);
            while (running) {
                bool isKeyframe = (timestamp % 1000) < 33;
                auto frame = isKeyframe ? createVideoKeyframe(timestamp) : createVideoInterFrame(timestamp);
                distribution_->distribute(testStreamKey_, frame);
                timestamp += 33;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& thread : threads) {
        thread.join();
    }

    // Test passes if no crashes
}

TEST_F(MediaDistributionConcurrencyTest, ConcurrentSubscriberModificationIsThreadSafe) {
    std::atomic<bool> running{true};
    std::atomic<size_t> addCount{0};
    std::atomic<size_t> removeCount{0};
    std::vector<std::thread> threads;

    // Producer thread - distributes frames
    threads.emplace_back([this, &running]() {
        uint32_t timestamp = 0;
        while (running) {
            distribution_->distribute(testStreamKey_, createVideoInterFrame(timestamp));
            timestamp += 33;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    // Add/remove subscriber threads
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([this, i, &running, &addCount, &removeCount]() {
            SubscriberId subId = static_cast<SubscriberId>(1000 + i * 100);
            while (running) {
                if (distribution_->addSubscriber(testStreamKey_, subId).isSuccess()) {
                    ++addCount;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                distribution_->removeSubscriber(subId);
                ++removeCount;
                subId++;
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& thread : threads) {
        thread.join();
    }

    // Test passes if no crashes
    EXPECT_GT(addCount.load(), 0u);
}

// =============================================================================
// Edge Cases Tests
// =============================================================================

TEST_F(MediaDistributionTest, HandleEmptyGOPBuffer) {
    // GOP buffer is empty (no frames)
    SubscriberId subscriberId = 100;

    auto result = distribution_->addSubscriber(testStreamKey_, subscriberId);

    // Should succeed but not send any video frames
    EXPECT_TRUE(result.isSuccess());
}

TEST_F(MediaDistributionTest, HandleNoKeyframeInBuffer) {
    // Push only inter frames (no keyframe)
    gopBuffer_->push(createVideoInterFrame(33));
    gopBuffer_->push(createVideoInterFrame(66));
    gopBuffer_->push(createVideoInterFrame(99));

    std::vector<BufferedFrame> sentFrames;
    distribution_->setOnFrameSentCallback(
        [&](SubscriberId, const BufferedFrame& frame) {
            sentFrames.push_back(frame);
        }
    );

    SubscriberId subscriberId = 100;
    distribution_->addSubscriber(testStreamKey_, subscriberId);

    // Should not send any video frames (no keyframe to start from)
    bool hasVideoFrame = false;
    for (const auto& frame : sentFrames) {
        if (frame.type == MediaType::Video && !frame.isKeyframe) {
            hasVideoFrame = true;
        }
    }
    EXPECT_FALSE(hasVideoFrame);
}

TEST_F(MediaDistributionTest, HandleZeroSubscribers) {
    // Distribute with no subscribers - should not crash
    EXPECT_NO_THROW(distribution_->distribute(testStreamKey_, createVideoKeyframe(0)));
}

TEST_F(MediaDistributionTest, MultipleStreamsIndependent) {
    // Create second stream
    StreamKey stream2Key("live", "test_stream_2");
    auto stream2IdResult = streamRegistry_->allocateStreamId();
    ASSERT_TRUE(stream2IdResult.isSuccess());
    streamRegistry_->registerStream(stream2Key, stream2IdResult.value(), 2);

    auto gopBuffer2 = std::make_shared<GOPBuffer>();
    distribution_->setGOPBuffer(stream2Key, gopBuffer2);

    // Add subscriber to each stream
    SubscriberId sub1 = 100;
    SubscriberId sub2 = 101;
    distribution_->addSubscriber(testStreamKey_, sub1);
    distribution_->addSubscriber(stream2Key, sub2);

    std::map<SubscriberId, int> frameCounts;
    distribution_->setOnFrameSentCallback(
        [&](SubscriberId subId, const BufferedFrame&) {
            frameCounts[subId]++;
        }
    );

    // Distribute to stream 1 only
    distribution_->distribute(testStreamKey_, createVideoKeyframe(0));

    // Only subscriber 1 should receive
    EXPECT_GE(frameCounts[sub1], 1);
    EXPECT_EQ(frameCounts[sub2], 0);
}

} // namespace test
} // namespace streaming
} // namespace openrtmp
