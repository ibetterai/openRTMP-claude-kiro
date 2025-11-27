// OpenRTMP - Cross-platform RTMP Server
// Tests for GOP Buffer
//
// Tests cover:
// - Maintain minimum 2 seconds of buffered media per stream
// - Index keyframe positions for instant playback start
// - Store metadata and codec sequence headers separately
// - Implement reference-counted buffer sharing for distribution
// - Support configurable buffer duration for low-latency mode
//
// Requirements coverage:
// - Requirement 4.6: Maintain GOP buffer of at least 2 seconds
// - Requirement 5.1: Transmit data starting from most recent keyframe
// - Requirement 5.3: Send cached metadata and sequence headers before stream data

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <cstdint>
#include <chrono>
#include <thread>
#include <atomic>

#include "openrtmp/streaming/gop_buffer.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace streaming {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class GOPBufferTest : public ::testing::Test {
protected:
    void SetUp() override {
        buffer_ = std::make_unique<GOPBuffer>();
    }

    void TearDown() override {
        buffer_.reset();
    }

    std::unique_ptr<GOPBuffer> buffer_;

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
        // Fill with some pattern for identification
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

    // Helper to create metadata AMFValue
    protocol::AMFValue createTestMetadata() {
        std::map<std::string, protocol::AMFValue> metadata;
        metadata["width"] = protocol::AMFValue::makeNumber(1920);
        metadata["height"] = protocol::AMFValue::makeNumber(1080);
        metadata["framerate"] = protocol::AMFValue::makeNumber(30);
        return protocol::AMFValue::makeObject(std::move(metadata));
    }
};

// =============================================================================
// Basic Buffer Operations Tests
// =============================================================================

TEST_F(GOPBufferTest, InitialStateIsEmpty) {
    EXPECT_EQ(buffer_->getBufferedDuration().count(), 0);
    EXPECT_EQ(buffer_->getBufferedBytes(), 0u);
    EXPECT_FALSE(buffer_->getMetadata().has_value());
}

TEST_F(GOPBufferTest, PushFrameAddsToBuffer) {
    auto frame = createVideoKeyframe(0);
    buffer_->push(frame);

    EXPECT_GT(buffer_->getBufferedBytes(), 0u);
}

TEST_F(GOPBufferTest, PushMultipleFramesUpdatesDuration) {
    // Create frames spanning 1000ms
    buffer_->push(createVideoKeyframe(0));
    buffer_->push(createVideoInterFrame(33));  // ~30fps
    buffer_->push(createVideoInterFrame(66));
    buffer_->push(createVideoInterFrame(100));
    buffer_->push(createVideoKeyframe(1000));  // 1 second later

    auto duration = buffer_->getBufferedDuration();
    EXPECT_GE(duration.count(), 1000);
}

TEST_F(GOPBufferTest, BufferedBytesAccumulatesCorrectly) {
    auto frame1 = createVideoKeyframe(0, 1000);
    auto frame2 = createVideoInterFrame(33, 500);
    auto frame3 = createAudioFrame(0, 200);

    buffer_->push(frame1);
    buffer_->push(frame2);
    buffer_->push(frame3);

    EXPECT_EQ(buffer_->getBufferedBytes(), 1700u);
}

// =============================================================================
// Minimum Buffer Duration Tests (Requirement 4.6)
// =============================================================================

TEST_F(GOPBufferTest, DefaultBufferDurationIsAtLeast2Seconds) {
    auto config = buffer_->getConfig();
    EXPECT_GE(config.minBufferDuration.count(), 2000);
}

TEST_F(GOPBufferTest, BufferMaintainsMinimum2SecondsOfMedia) {
    // Push 4 seconds worth of frames at 30fps (120 frames)
    uint32_t timestamp = 0;
    for (int gop = 0; gop < 4; ++gop) {
        // Each GOP is 1 second
        buffer_->push(createVideoKeyframe(timestamp, 5000));
        for (int frame = 1; frame < 30; ++frame) {
            timestamp += 33;
            buffer_->push(createVideoInterFrame(timestamp, 1000));
        }
        timestamp += 33;
    }

    // Buffer should maintain at least 2 seconds
    auto duration = buffer_->getBufferedDuration();
    EXPECT_GE(duration.count(), 2000);
}

TEST_F(GOPBufferTest, CircularBufferEvictsOldFrames) {
    // Set a small buffer size for testing
    GOPBufferConfig config;
    config.minBufferDuration = std::chrono::milliseconds(2000);
    config.maxBufferSize = 100000;  // 100KB max
    buffer_->setConfig(config);

    // Push enough frames to exceed buffer
    uint32_t timestamp = 0;
    for (int i = 0; i < 200; ++i) {
        if (i % 30 == 0) {
            buffer_->push(createVideoKeyframe(timestamp, 5000));
        } else {
            buffer_->push(createVideoInterFrame(timestamp, 1000));
        }
        timestamp += 33;
    }

    // Buffer should not exceed max size
    EXPECT_LE(buffer_->getBufferedBytes(), config.maxBufferSize);
}

// =============================================================================
// Keyframe Indexing Tests (Requirement 5.1)
// =============================================================================

TEST_F(GOPBufferTest, TracksKeyframePositions) {
    buffer_->push(createVideoKeyframe(0));
    buffer_->push(createVideoInterFrame(33));
    buffer_->push(createVideoInterFrame(66));
    buffer_->push(createVideoKeyframe(1000));
    buffer_->push(createVideoInterFrame(1033));

    auto frames = buffer_->getFromLastKeyframe();

    // Should return from keyframe at 1000ms
    ASSERT_FALSE(frames.empty());
    EXPECT_TRUE(frames[0].isKeyframe);
    EXPECT_EQ(frames[0].timestamp, 1000u);
}

TEST_F(GOPBufferTest, GetFromLastKeyframeReturnsEntireGOP) {
    buffer_->push(createVideoKeyframe(0));
    buffer_->push(createVideoInterFrame(33));
    buffer_->push(createVideoInterFrame(66));
    buffer_->push(createVideoKeyframe(1000));
    buffer_->push(createVideoInterFrame(1033));
    buffer_->push(createVideoInterFrame(1066));
    buffer_->push(createAudioFrame(1000));
    buffer_->push(createAudioFrame(1033));

    auto frames = buffer_->getFromLastKeyframe();

    // Should include keyframe + 2 inter frames + 2 audio frames from same GOP
    EXPECT_GE(frames.size(), 3u);

    // First frame should be keyframe
    EXPECT_TRUE(frames[0].isKeyframe);
    EXPECT_EQ(frames[0].timestamp, 1000u);
}

TEST_F(GOPBufferTest, GetFromLastKeyframeReturnsEmptyIfNoKeyframe) {
    // Only push inter frames
    buffer_->push(createVideoInterFrame(0));
    buffer_->push(createVideoInterFrame(33));
    buffer_->push(createVideoInterFrame(66));

    auto frames = buffer_->getFromLastKeyframe();

    EXPECT_TRUE(frames.empty());
}

TEST_F(GOPBufferTest, NewKeyframeUpdatesLastKeyframeIndex) {
    buffer_->push(createVideoKeyframe(0));
    buffer_->push(createVideoInterFrame(33));
    buffer_->push(createVideoKeyframe(1000));
    buffer_->push(createVideoInterFrame(1033));
    buffer_->push(createVideoKeyframe(2000));
    buffer_->push(createVideoInterFrame(2033));

    auto frames = buffer_->getFromLastKeyframe();

    // Should return from most recent keyframe at 2000ms
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(frames[0].timestamp, 2000u);
}

// =============================================================================
// Metadata and Sequence Header Tests (Requirement 5.3)
// =============================================================================

TEST_F(GOPBufferTest, SetAndGetMetadata) {
    auto metadata = createTestMetadata();
    buffer_->setMetadata(metadata);

    auto retrieved = buffer_->getMetadata();

    ASSERT_TRUE(retrieved.has_value());
    EXPECT_TRUE(retrieved->isObject());

    auto& obj = retrieved->asObject();
    EXPECT_DOUBLE_EQ(obj.at("width").asNumber(), 1920.0);
    EXPECT_DOUBLE_EQ(obj.at("height").asNumber(), 1080.0);
}

TEST_F(GOPBufferTest, SetAndGetSequenceHeaders) {
    std::vector<uint8_t> videoHeader = {0x17, 0x00, 0x00, 0x00, 0x00, 0x01, 0x64};
    std::vector<uint8_t> audioHeader = {0xAF, 0x00, 0x12, 0x10};

    buffer_->setSequenceHeaders(videoHeader, audioHeader);

    auto headers = buffer_->getSequenceHeaders();

    EXPECT_EQ(headers.videoHeader, videoHeader);
    EXPECT_EQ(headers.audioHeader, audioHeader);
}

TEST_F(GOPBufferTest, SequenceHeadersAreStoredSeparately) {
    std::vector<uint8_t> videoHeader = {0x17, 0x00, 0x00, 0x00, 0x00};
    std::vector<uint8_t> audioHeader = {0xAF, 0x00};

    buffer_->setSequenceHeaders(videoHeader, audioHeader);

    // Push frames and evict them
    uint32_t timestamp = 0;
    for (int i = 0; i < 100; ++i) {
        buffer_->push(createVideoKeyframe(timestamp, 10000));
        timestamp += 1000;
    }

    // Headers should still be available
    auto headers = buffer_->getSequenceHeaders();
    EXPECT_EQ(headers.videoHeader, videoHeader);
    EXPECT_EQ(headers.audioHeader, audioHeader);
}

TEST_F(GOPBufferTest, MetadataIsStoredSeparately) {
    auto metadata = createTestMetadata();
    buffer_->setMetadata(metadata);

    // Push frames and evict them
    uint32_t timestamp = 0;
    for (int i = 0; i < 100; ++i) {
        buffer_->push(createVideoKeyframe(timestamp, 10000));
        timestamp += 1000;
    }

    // Metadata should still be available
    auto retrieved = buffer_->getMetadata();
    ASSERT_TRUE(retrieved.has_value());
}

TEST_F(GOPBufferTest, EmptySequenceHeadersReturnsEmptyVectors) {
    auto headers = buffer_->getSequenceHeaders();

    EXPECT_TRUE(headers.videoHeader.empty());
    EXPECT_TRUE(headers.audioHeader.empty());
}

// =============================================================================
// Reference-Counted Buffer Sharing Tests
// =============================================================================

TEST_F(GOPBufferTest, GetFromLastKeyframeReturnsSharedData) {
    buffer_->push(createVideoKeyframe(0, 1000));
    buffer_->push(createVideoInterFrame(33, 500));

    auto frames1 = buffer_->getFromLastKeyframe();
    auto frames2 = buffer_->getFromLastKeyframe();

    // Both should have same data
    ASSERT_FALSE(frames1.empty());
    ASSERT_FALSE(frames2.empty());
    EXPECT_EQ(frames1[0].data.size(), frames2[0].data.size());
    EXPECT_EQ(frames1[0].data, frames2[0].data);
}

TEST_F(GOPBufferTest, FrameCountReturnsCorrectValue) {
    buffer_->push(createVideoKeyframe(0));
    buffer_->push(createVideoInterFrame(33));
    buffer_->push(createVideoInterFrame(66));
    buffer_->push(createAudioFrame(0));
    buffer_->push(createAudioFrame(33));

    EXPECT_EQ(buffer_->getFrameCount(), 5u);
}

// =============================================================================
// Configurable Buffer Duration Tests (Low-Latency Mode)
// =============================================================================

TEST_F(GOPBufferTest, SetConfigChangesBufferBehavior) {
    GOPBufferConfig config;
    config.minBufferDuration = std::chrono::milliseconds(500);  // Low-latency
    config.maxBufferSize = 50000;

    buffer_->setConfig(config);

    auto retrievedConfig = buffer_->getConfig();
    EXPECT_EQ(retrievedConfig.minBufferDuration.count(), 500);
    EXPECT_EQ(retrievedConfig.maxBufferSize, 50000u);
}

TEST_F(GOPBufferTest, LowLatencyModeReducesBuffer) {
    GOPBufferConfig config;
    config.minBufferDuration = std::chrono::milliseconds(500);
    config.maxBufferSize = 50000;
    buffer_->setConfig(config);

    // Push 3 seconds worth of frames
    uint32_t timestamp = 0;
    for (int gop = 0; gop < 3; ++gop) {
        buffer_->push(createVideoKeyframe(timestamp, 2000));
        for (int frame = 1; frame < 30; ++frame) {
            timestamp += 33;
            buffer_->push(createVideoInterFrame(timestamp, 500));
        }
        timestamp += 33;
    }

    // Buffer should be limited by low-latency config
    auto duration = buffer_->getBufferedDuration();
    // Should be roughly around minBufferDuration, not more than 2 GOPs
    EXPECT_LE(duration.count(), 2000);
}

TEST_F(GOPBufferTest, DefaultConfigUsesStandardValues) {
    auto config = buffer_->getConfig();

    EXPECT_GE(config.minBufferDuration.count(), 2000);  // At least 2 seconds
    EXPECT_GT(config.maxBufferSize, 0u);
}

// =============================================================================
// Clear and Reset Tests
// =============================================================================

TEST_F(GOPBufferTest, ClearRemovesAllFrames) {
    buffer_->push(createVideoKeyframe(0));
    buffer_->push(createVideoInterFrame(33));
    buffer_->setMetadata(createTestMetadata());
    buffer_->setSequenceHeaders({0x17}, {0xAF});

    buffer_->clear();

    EXPECT_EQ(buffer_->getBufferedBytes(), 0u);
    EXPECT_EQ(buffer_->getBufferedDuration().count(), 0);
    EXPECT_EQ(buffer_->getFrameCount(), 0u);
}

TEST_F(GOPBufferTest, ClearPreservesMetadataAndHeaders) {
    buffer_->push(createVideoKeyframe(0));
    auto metadata = createTestMetadata();
    buffer_->setMetadata(metadata);
    buffer_->setSequenceHeaders({0x17}, {0xAF});

    buffer_->clear();

    // Metadata and headers should still be available
    EXPECT_TRUE(buffer_->getMetadata().has_value());
    auto headers = buffer_->getSequenceHeaders();
    EXPECT_FALSE(headers.videoHeader.empty());
}

TEST_F(GOPBufferTest, ResetClearsEverything) {
    buffer_->push(createVideoKeyframe(0));
    buffer_->setMetadata(createTestMetadata());
    buffer_->setSequenceHeaders({0x17}, {0xAF});

    buffer_->reset();

    EXPECT_EQ(buffer_->getBufferedBytes(), 0u);
    EXPECT_FALSE(buffer_->getMetadata().has_value());
    auto headers = buffer_->getSequenceHeaders();
    EXPECT_TRUE(headers.videoHeader.empty());
    EXPECT_TRUE(headers.audioHeader.empty());
}

// =============================================================================
// Edge Cases Tests
// =============================================================================

TEST_F(GOPBufferTest, HandlesSingleFrame) {
    auto frame = createVideoKeyframe(0);
    buffer_->push(frame);

    auto frames = buffer_->getFromLastKeyframe();

    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].timestamp, 0u);
    EXPECT_TRUE(frames[0].isKeyframe);
}

TEST_F(GOPBufferTest, HandlesRapidKeyframes) {
    // Push multiple keyframes in quick succession
    for (int i = 0; i < 10; ++i) {
        buffer_->push(createVideoKeyframe(static_cast<uint32_t>(i * 100)));
    }

    auto frames = buffer_->getFromLastKeyframe();

    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(frames[0].timestamp, 900u);
}

TEST_F(GOPBufferTest, HandlesInterleavedAudioVideo) {
    // Interleaved audio and video as in real streams
    buffer_->push(createVideoKeyframe(0));
    buffer_->push(createAudioFrame(0));
    buffer_->push(createAudioFrame(21));
    buffer_->push(createVideoInterFrame(33));
    buffer_->push(createAudioFrame(42));
    buffer_->push(createVideoInterFrame(66));
    buffer_->push(createAudioFrame(63));
    buffer_->push(createVideoKeyframe(1000));
    buffer_->push(createAudioFrame(1000));
    buffer_->push(createVideoInterFrame(1033));
    buffer_->push(createAudioFrame(1021));

    auto frames = buffer_->getFromLastKeyframe();

    // Should get frames from keyframe at 1000ms onwards
    ASSERT_GE(frames.size(), 2u);
    EXPECT_GE(frames[0].timestamp, 1000u);
}

TEST_F(GOPBufferTest, HandlesZeroTimestamp) {
    buffer_->push(createVideoKeyframe(0));

    auto frames = buffer_->getFromLastKeyframe();

    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].timestamp, 0u);
}

TEST_F(GOPBufferTest, HandlesLargeTimestampValues) {
    // Test with large timestamp values (near uint32_t max)
    uint32_t largeTs = 0xFFFFFF00;
    buffer_->push(createVideoKeyframe(largeTs));
    buffer_->push(createVideoInterFrame(largeTs + 33));

    auto frames = buffer_->getFromLastKeyframe();

    ASSERT_GE(frames.size(), 1u);
    EXPECT_EQ(frames[0].timestamp, largeTs);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

class GOPBufferConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        buffer_ = std::make_unique<GOPBuffer>();
    }

    std::unique_ptr<GOPBuffer> buffer_;
};

TEST_F(GOPBufferConcurrencyTest, ConcurrentPushIsThreadSafe) {
    const int numThreads = 4;
    const int framesPerThread = 100;
    std::vector<std::thread> threads;

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([this, t]() {
            for (int i = 0; i < 100; ++i) {
                BufferedFrame frame;
                frame.type = MediaType::Video;
                frame.timestamp = static_cast<uint32_t>(t * 10000 + i * 33);
                frame.isKeyframe = (i % 30 == 0);
                frame.data.resize(100);
                buffer_->push(frame);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // No crashes and buffer has data
    EXPECT_GT(buffer_->getFrameCount(), 0u);
}

TEST_F(GOPBufferConcurrencyTest, ConcurrentReadWriteIsThreadSafe) {
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;

    // Writer thread
    threads.emplace_back([this, &running]() {
        uint32_t timestamp = 0;
        while (running) {
            BufferedFrame frame;
            frame.type = MediaType::Video;
            frame.timestamp = timestamp;
            frame.isKeyframe = (timestamp % 1000 == 0);
            frame.data.resize(100);
            buffer_->push(frame);
            timestamp += 33;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // Reader threads
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([this, &running]() {
            while (running) {
                buffer_->getFromLastKeyframe();
                buffer_->getBufferedDuration();
                buffer_->getBufferedBytes();
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
}

TEST_F(GOPBufferConcurrencyTest, ConcurrentMetadataAccessIsThreadSafe) {
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;

    // Writer thread
    threads.emplace_back([this, &running]() {
        int counter = 0;
        while (running) {
            std::map<std::string, protocol::AMFValue> metadata;
            metadata["counter"] = protocol::AMFValue::makeNumber(static_cast<double>(counter++));
            buffer_->setMetadata(protocol::AMFValue::makeObject(std::move(metadata)));
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // Reader threads
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([this, &running]() {
            while (running) {
                buffer_->getMetadata();
                buffer_->getSequenceHeaders();
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

} // namespace test
} // namespace streaming
} // namespace openrtmp
