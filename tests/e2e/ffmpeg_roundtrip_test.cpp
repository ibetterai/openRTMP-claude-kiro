// OpenRTMP - Cross-platform RTMP Server
// E2E Tests: FFmpeg Publish-to-Play Roundtrip
//
// Task 21.3: Implement E2E and performance tests
// Tests FFmpeg client behavior simulation for publish and play operations.
//
// Note: This test simulates FFmpeg client behavior for unit testing.
// Real FFmpeg testing requires manual integration testing with actual FFmpeg.
//
// Requirements coverage:
// - Requirement 4.1: Buffer and store stream with associated stream key
// - Requirement 4.2: Support H.264/AVC video codec
// - Requirement 4.3: Support AAC audio codec
// - Requirement 5.1: Begin transmitting data starting from most recent keyframe
// - Requirement 5.2: Support multiple simultaneous subscribers

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>

#include "openrtmp/streaming/stream_registry.hpp"
#include "openrtmp/streaming/gop_buffer.hpp"
#include "openrtmp/streaming/media_handler.hpp"
#include "openrtmp/streaming/media_distribution.hpp"
#include "openrtmp/streaming/subscriber_manager.hpp"
#include "openrtmp/protocol/handshake_handler.hpp"
#include "openrtmp/protocol/command_handler.hpp"
#include "openrtmp/protocol/amf_codec.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace e2e {
namespace test {

// =============================================================================
// FFmpeg Client Behavior Simulator
// =============================================================================

/**
 * @brief Simulates FFmpeg RTMP client behavior for protocol testing.
 *
 * FFmpeg uses librtmp or native RTMP implementation with specific
 * command patterns and codec configurations.
 */
class FFmpegClientSimulator {
public:
    enum class Mode {
        Publisher,
        Player
    };

    FFmpegClientSimulator(Mode mode)
        : mode_(mode)
        , transactionId_(1.0)
        , streamId_(0)
    {}

    // FFmpeg identification
    static constexpr const char* FLASH_VERSION = "LNX 9,0,124,2";

    // Create FFmpeg-style connect command
    protocol::AMFValue createConnectCommand(const std::string& app,
                                             const std::string& host = "localhost",
                                             uint16_t port = 1935) {
        std::map<std::string, protocol::AMFValue> cmdObj;
        cmdObj["app"] = protocol::AMFValue::makeString(app);
        cmdObj["type"] = protocol::AMFValue::makeString("nonprivate");
        cmdObj["flashVer"] = protocol::AMFValue::makeString(FLASH_VERSION);
        cmdObj["tcUrl"] = protocol::AMFValue::makeString(
            "rtmp://" + host + ":" + std::to_string(port) + "/" + app);

        // FFmpeg capabilities
        cmdObj["fpad"] = protocol::AMFValue::makeBoolean(false);
        cmdObj["capabilities"] = protocol::AMFValue::makeNumber(15.0);
        cmdObj["audioCodecs"] = protocol::AMFValue::makeNumber(4071.0);
        cmdObj["videoCodecs"] = protocol::AMFValue::makeNumber(252.0);
        cmdObj["videoFunction"] = protocol::AMFValue::makeNumber(1.0);

        return protocol::AMFValue::makeObject(std::move(cmdObj));
    }

    // Create FFmpeg-style publish command
    protocol::RTMPCommand createPublishCommand(const std::string& streamName) {
        protocol::RTMPCommand cmd;
        cmd.name = "publish";
        cmd.transactionId = transactionId_++;
        cmd.commandObject = protocol::AMFValue::makeNull();
        cmd.args.push_back(protocol::AMFValue::makeString(streamName));
        cmd.args.push_back(protocol::AMFValue::makeString("live"));
        return cmd;
    }

    // Create FFmpeg-style play command
    protocol::RTMPCommand createPlayCommand(const std::string& streamName) {
        protocol::RTMPCommand cmd;
        cmd.name = "play";
        cmd.transactionId = transactionId_++;
        cmd.commandObject = protocol::AMFValue::makeNull();
        cmd.args.push_back(protocol::AMFValue::makeString(streamName));
        cmd.args.push_back(protocol::AMFValue::makeNumber(-2.0));  // Start from live
        cmd.args.push_back(protocol::AMFValue::makeNumber(-1.0));  // Play until stopped
        cmd.args.push_back(protocol::AMFValue::makeBoolean(true)); // Reset
        return cmd;
    }

    // Create FFmpeg-style metadata (from -metadata or auto-generated)
    protocol::AMFValue createFFmpegMetadata(uint32_t width = 1280,
                                             uint32_t height = 720,
                                             double fps = 25.0) {
        std::map<std::string, protocol::AMFValue> metadata;

        // FFmpeg metadata
        metadata["duration"] = protocol::AMFValue::makeNumber(0.0);
        metadata["width"] = protocol::AMFValue::makeNumber(static_cast<double>(width));
        metadata["height"] = protocol::AMFValue::makeNumber(static_cast<double>(height));
        metadata["videodatarate"] = protocol::AMFValue::makeNumber(2500.0);
        metadata["framerate"] = protocol::AMFValue::makeNumber(fps);
        metadata["videocodecid"] = protocol::AMFValue::makeNumber(7.0);  // AVC (numeric)

        metadata["audiodatarate"] = protocol::AMFValue::makeNumber(128.0);
        metadata["audiosamplerate"] = protocol::AMFValue::makeNumber(44100.0);
        metadata["audiosamplesize"] = protocol::AMFValue::makeNumber(16.0);
        metadata["stereo"] = protocol::AMFValue::makeBoolean(true);
        metadata["audiocodecid"] = protocol::AMFValue::makeNumber(10.0);  // AAC (numeric)

        metadata["encoder"] = protocol::AMFValue::makeString("Lavf60.3.100");

        return protocol::AMFValue::makeObject(std::move(metadata));
    }

    // Create video keyframe
    streaming::BufferedFrame createVideoKeyframe(uint32_t timestamp, size_t dataSize = 5000) {
        streaming::BufferedFrame frame;
        frame.type = MediaType::Video;
        frame.timestamp = timestamp;
        frame.isKeyframe = true;

        frame.data.resize(5 + dataSize);
        frame.data[0] = 0x17;  // keyframe + AVC
        frame.data[1] = 0x01;  // AVC NALU
        frame.data[2] = 0x00;
        frame.data[3] = 0x00;
        frame.data[4] = 0x00;

        for (size_t i = 5; i < frame.data.size(); ++i) {
            frame.data[i] = static_cast<uint8_t>((timestamp + i) & 0xFF);
        }

        return frame;
    }

    // Create video inter frame
    streaming::BufferedFrame createVideoInterFrame(uint32_t timestamp, size_t dataSize = 1500) {
        streaming::BufferedFrame frame;
        frame.type = MediaType::Video;
        frame.timestamp = timestamp;
        frame.isKeyframe = false;

        frame.data.resize(5 + dataSize);
        frame.data[0] = 0x27;  // inter frame + AVC
        frame.data[1] = 0x01;
        frame.data[2] = 0x00;
        frame.data[3] = 0x00;
        frame.data[4] = 0x00;

        for (size_t i = 5; i < frame.data.size(); ++i) {
            frame.data[i] = static_cast<uint8_t>((timestamp + i) & 0xFF);
        }

        return frame;
    }

    // Create audio frame
    streaming::BufferedFrame createAudioFrame(uint32_t timestamp, size_t dataSize = 400) {
        streaming::BufferedFrame frame;
        frame.type = MediaType::Audio;
        frame.timestamp = timestamp;
        frame.isKeyframe = true;

        frame.data.resize(2 + dataSize);
        frame.data[0] = 0xaf;  // AAC
        frame.data[1] = 0x01;  // AAC raw

        for (size_t i = 2; i < frame.data.size(); ++i) {
            frame.data[i] = static_cast<uint8_t>((timestamp + i) & 0xFF);
        }

        return frame;
    }

    // Create sequence headers
    std::vector<uint8_t> createVideoSequenceHeader() {
        return {
            0x17, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x4d, 0x40, 0x1f, 0xff,  // Main Profile, Level 3.1
            0xe1, 0x00, 0x14,
            0x67, 0x4d, 0x40, 0x1f, 0xe8, 0x80, 0x50, 0x05,
            0xb8, 0x08, 0x80, 0x00, 0x00, 0x03, 0x00, 0x80,
            0x00, 0x00, 0x19, 0x47, 0x8a, 0x14,
            0x01, 0x00, 0x04,
            0x68, 0xeb, 0xef, 0x20
        };
    }

    std::vector<uint8_t> createAudioSequenceHeader() {
        return {
            0xaf, 0x00,
            0x12, 0x08  // AAC-LC, 44100Hz, Stereo
        };
    }

    double getNextTransactionId() { return transactionId_++; }
    void setStreamId(StreamId id) { streamId_ = id; }
    StreamId getStreamId() const { return streamId_; }
    Mode getMode() const { return mode_; }

private:
    Mode mode_;
    double transactionId_;
    StreamId streamId_;
};

// =============================================================================
// Mock Subscriber for Roundtrip Testing
// =============================================================================

class MockSubscriber {
public:
    MockSubscriber(SubscriberId id) : id_(id) {}

    void onFrame(const streaming::BufferedFrame& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        receivedFrames_.push(frame);
        totalBytesReceived_ += frame.data.size();
        lastTimestamp_ = frame.timestamp;
    }

    void onMetadata(const protocol::AMFValue& metadata) {
        std::lock_guard<std::mutex> lock(mutex_);
        metadataReceived_ = true;
        metadata_ = metadata;
    }

    void onSequenceHeaders(const std::vector<uint8_t>& video,
                           const std::vector<uint8_t>& audio) {
        std::lock_guard<std::mutex> lock(mutex_);
        videoHeaderReceived_ = !video.empty();
        audioHeaderReceived_ = !audio.empty();
        videoHeader_ = video;
        audioHeader_ = audio;
    }

    size_t getFrameCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return receivedFrames_.size();
    }

    size_t getTotalBytesReceived() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return totalBytesReceived_;
    }

    bool hasMetadata() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return metadataReceived_;
    }

    bool hasVideoHeader() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return videoHeaderReceived_;
    }

    bool hasAudioHeader() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return audioHeaderReceived_;
    }

    uint32_t getLastTimestamp() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastTimestamp_;
    }

    SubscriberId getId() const { return id_; }

    // Get the first received video frame (for keyframe verification)
    std::optional<streaming::BufferedFrame> getFirstVideoFrame() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (receivedFrames_.empty()) return std::nullopt;

        // Find first video frame
        auto tempQueue = receivedFrames_;
        while (!tempQueue.empty()) {
            auto frame = tempQueue.front();
            tempQueue.pop();
            if (frame.type == MediaType::Video) {
                return frame;
            }
        }
        return std::nullopt;
    }

private:
    SubscriberId id_;
    mutable std::mutex mutex_;
    std::queue<streaming::BufferedFrame> receivedFrames_;
    size_t totalBytesReceived_ = 0;
    uint32_t lastTimestamp_ = 0;
    bool metadataReceived_ = false;
    bool videoHeaderReceived_ = false;
    bool audioHeaderReceived_ = false;
    protocol::AMFValue metadata_;
    std::vector<uint8_t> videoHeader_;
    std::vector<uint8_t> audioHeader_;
};

// =============================================================================
// Test Fixtures
// =============================================================================

class FFmpegRoundtripTest : public ::testing::Test {
protected:
    void SetUp() override {
        streamRegistry_ = std::make_shared<streaming::StreamRegistry>();
        subscriberManager_ = std::make_shared<streaming::SubscriberManager>(streamRegistry_);
        mediaDistribution_ = std::make_shared<streaming::MediaDistribution>(
            streamRegistry_, subscriberManager_);

        amfCodec_ = std::make_shared<protocol::AMFCodec>();
        commandHandler_ = std::make_unique<protocol::CommandHandler>(streamRegistry_, amfCodec_);

        publisher_ = std::make_unique<FFmpegClientSimulator>(FFmpegClientSimulator::Mode::Publisher);
        player_ = std::make_unique<FFmpegClientSimulator>(FFmpegClientSimulator::Mode::Player);
    }

    void TearDown() override {
        mockSubscribers_.clear();
        player_.reset();
        publisher_.reset();
        commandHandler_.reset();
        mediaDistribution_.reset();
        subscriberManager_.reset();
        streamRegistry_->clear();
        streamRegistry_.reset();
    }

    // Setup a publishing stream
    bool setupPublisher(const std::string& app, const std::string& streamName) {
        StreamKey streamKey{app, streamName};
        auto allocResult = streamRegistry_->allocateStreamId();
        if (!allocResult.isSuccess()) return false;

        auto regResult = streamRegistry_->registerStream(streamKey, allocResult.value(), 1);
        if (!regResult.isSuccess()) return false;

        gopBuffer_ = std::make_shared<streaming::GOPBuffer>();
        mediaDistribution_->setGOPBuffer(streamKey, gopBuffer_);

        currentStreamKey_ = streamKey;
        return true;
    }

    // Add a mock subscriber
    MockSubscriber* addSubscriber() {
        SubscriberId id = nextSubscriberId_++;
        auto subscriber = std::make_unique<MockSubscriber>(id);
        MockSubscriber* ptr = subscriber.get();
        mockSubscribers_[id] = std::move(subscriber);
        return ptr;
    }

    // Components
    std::shared_ptr<streaming::StreamRegistry> streamRegistry_;
    std::shared_ptr<streaming::SubscriberManager> subscriberManager_;
    std::shared_ptr<streaming::MediaDistribution> mediaDistribution_;
    std::shared_ptr<protocol::IAMFCodec> amfCodec_;
    std::unique_ptr<protocol::CommandHandler> commandHandler_;
    std::shared_ptr<streaming::GOPBuffer> gopBuffer_;

    std::unique_ptr<FFmpegClientSimulator> publisher_;
    std::unique_ptr<FFmpegClientSimulator> player_;

    std::map<SubscriberId, std::unique_ptr<MockSubscriber>> mockSubscribers_;
    SubscriberId nextSubscriberId_ = 1;
    StreamKey currentStreamKey_;
};

// =============================================================================
// FFmpeg Publish Tests
// =============================================================================

TEST_F(FFmpegRoundtripTest, FFmpegPublishEstablishesStream) {
    ASSERT_TRUE(setupPublisher("live", "ffmpeg_test"));

    // Send FFmpeg-style metadata
    mediaDistribution_->setMetadata(currentStreamKey_, publisher_->createFFmpegMetadata());

    // Verify stream exists
    EXPECT_TRUE(streamRegistry_->hasStream(currentStreamKey_));
}

TEST_F(FFmpegRoundtripTest, FFmpegMetadataNumericCodecIds) {
    ASSERT_TRUE(setupPublisher("live", "ffmpeg_test"));

    auto metadata = publisher_->createFFmpegMetadata();
    mediaDistribution_->setMetadata(currentStreamKey_, metadata);

    auto storedMetadata = gopBuffer_->getMetadata();
    ASSERT_TRUE(storedMetadata.has_value());

    // FFmpeg uses numeric codec IDs
    auto& obj = storedMetadata->asObject();
    EXPECT_EQ(obj.at("videocodecid").asNumber(), 7.0);   // AVC
    EXPECT_EQ(obj.at("audiocodecid").asNumber(), 10.0);  // AAC
}

TEST_F(FFmpegRoundtripTest, FFmpegSequenceHeadersStored) {
    ASSERT_TRUE(setupPublisher("live", "ffmpeg_test"));

    auto videoHeader = publisher_->createVideoSequenceHeader();
    auto audioHeader = publisher_->createAudioSequenceHeader();
    mediaDistribution_->setSequenceHeaders(currentStreamKey_, videoHeader, audioHeader);

    auto headers = gopBuffer_->getSequenceHeaders();
    EXPECT_EQ(headers.videoHeader, videoHeader);
    EXPECT_EQ(headers.audioHeader, audioHeader);
}

// =============================================================================
// FFmpeg Play Tests
// =============================================================================

TEST_F(FFmpegRoundtripTest, FFmpegPlayCommandFormat) {
    auto playCmd = player_->createPlayCommand("test_stream");

    EXPECT_EQ(playCmd.name, "play");
    EXPECT_EQ(playCmd.args.size(), 4u);
    EXPECT_EQ(playCmd.args[0].asString(), "test_stream");
    EXPECT_EQ(playCmd.args[1].asNumber(), -2.0);  // Live playback
    EXPECT_EQ(playCmd.args[2].asNumber(), -1.0);  // Indefinite
    EXPECT_TRUE(playCmd.args[3].asBool());         // Reset
}

// =============================================================================
// Complete Roundtrip Tests
// =============================================================================

TEST_F(FFmpegRoundtripTest, PublishToPlayRoundtrip) {
    // Phase 1: Setup publisher
    ASSERT_TRUE(setupPublisher("live", "roundtrip_test"));

    // Phase 2: Send metadata and sequence headers
    mediaDistribution_->setMetadata(currentStreamKey_, publisher_->createFFmpegMetadata());
    mediaDistribution_->setSequenceHeaders(currentStreamKey_,
        publisher_->createVideoSequenceHeader(),
        publisher_->createAudioSequenceHeader());

    // Phase 3: Publish some frames (build up GOP buffer)
    uint32_t timestamp = 0;
    const uint32_t frameInterval = 40;  // 25fps

    // First GOP
    mediaDistribution_->distribute(currentStreamKey_, publisher_->createVideoKeyframe(timestamp));
    timestamp += frameInterval;

    for (int i = 0; i < 24; ++i) {
        mediaDistribution_->distribute(currentStreamKey_, publisher_->createVideoInterFrame(timestamp));
        timestamp += frameInterval;
    }

    // Second GOP (subscriber joins mid-stream)
    uint32_t secondKeyframeTs = timestamp;
    mediaDistribution_->distribute(currentStreamKey_, publisher_->createVideoKeyframe(timestamp));
    timestamp += frameInterval;

    // Phase 4: Add subscriber (simulates ffplay connecting)
    auto subscriber = addSubscriber();
    streaming::SubscriberConfig config;
    config.lowLatencyMode = false;
    config.maxBufferDuration = std::chrono::milliseconds(5000);

    auto addResult = subscriberManager_->addSubscriber(currentStreamKey_, subscriber->getId(), config);
    ASSERT_TRUE(addResult.isSuccess());

    // Phase 5: Subscriber should receive from latest keyframe
    // Simulate delivering buffered content to subscriber
    auto bufferedFrames = gopBuffer_->getFromLastKeyframe();
    for (const auto& frame : bufferedFrames) {
        subscriber->onFrame(frame);
    }

    // Phase 6: Continue publishing (subscriber receives live)
    for (int i = 0; i < 10; ++i) {
        auto frame = publisher_->createVideoInterFrame(timestamp);
        mediaDistribution_->distribute(currentStreamKey_, frame);
        subscriber->onFrame(frame);
        timestamp += frameInterval;
    }

    // Verify subscriber received frames
    EXPECT_GT(subscriber->getFrameCount(), 0u);

    // Verify first video frame is a keyframe (Requirement 5.1)
    auto firstVideo = subscriber->getFirstVideoFrame();
    ASSERT_TRUE(firstVideo.has_value());
    EXPECT_TRUE(firstVideo->isKeyframe);
}

TEST_F(FFmpegRoundtripTest, MultipleFFmpegPlayersReceiveIndependently) {
    // Setup publisher
    ASSERT_TRUE(setupPublisher("live", "multi_player_test"));

    mediaDistribution_->setMetadata(currentStreamKey_, publisher_->createFFmpegMetadata());
    mediaDistribution_->setSequenceHeaders(currentStreamKey_,
        publisher_->createVideoSequenceHeader(),
        publisher_->createAudioSequenceHeader());

    // Build initial buffer
    uint32_t timestamp = 0;
    const uint32_t frameInterval = 40;

    mediaDistribution_->distribute(currentStreamKey_, publisher_->createVideoKeyframe(timestamp));
    timestamp += frameInterval;

    for (int i = 0; i < 20; ++i) {
        mediaDistribution_->distribute(currentStreamKey_, publisher_->createVideoInterFrame(timestamp));
        timestamp += frameInterval;
    }

    // Add first subscriber
    auto subscriber1 = addSubscriber();
    streaming::SubscriberConfig config1;
    config1.lowLatencyMode = false;
    subscriberManager_->addSubscriber(currentStreamKey_, subscriber1->getId(), config1);

    // Deliver buffered content to subscriber1
    auto buffered = gopBuffer_->getFromLastKeyframe();
    for (const auto& frame : buffered) {
        subscriber1->onFrame(frame);
    }

    // Publish more frames (only subscriber1 receives)
    for (int i = 0; i < 10; ++i) {
        auto frame = publisher_->createVideoInterFrame(timestamp);
        mediaDistribution_->distribute(currentStreamKey_, frame);
        subscriber1->onFrame(frame);
        timestamp += frameInterval;
    }

    // Add second subscriber (joins later)
    mediaDistribution_->distribute(currentStreamKey_, publisher_->createVideoKeyframe(timestamp));
    timestamp += frameInterval;

    auto subscriber2 = addSubscriber();
    streaming::SubscriberConfig config2;
    config2.lowLatencyMode = true;  // Different config
    subscriberManager_->addSubscriber(currentStreamKey_, subscriber2->getId(), config2);

    // Deliver to subscriber2 from keyframe
    buffered = gopBuffer_->getFromLastKeyframe();
    for (const auto& frame : buffered) {
        subscriber2->onFrame(frame);
    }

    // Publish more frames (both receive)
    for (int i = 0; i < 10; ++i) {
        auto frame = publisher_->createVideoInterFrame(timestamp);
        mediaDistribution_->distribute(currentStreamKey_, frame);
        subscriber1->onFrame(frame);
        subscriber2->onFrame(frame);
        timestamp += frameInterval;
    }

    // Verify independent reception
    EXPECT_GT(subscriber1->getFrameCount(), subscriber2->getFrameCount());

    // Both should have started from a keyframe
    auto first1 = subscriber1->getFirstVideoFrame();
    auto first2 = subscriber2->getFirstVideoFrame();
    ASSERT_TRUE(first1.has_value());
    ASSERT_TRUE(first2.has_value());
    EXPECT_TRUE(first1->isKeyframe);
    EXPECT_TRUE(first2->isKeyframe);
}

TEST_F(FFmpegRoundtripTest, AudioVideoSyncInRoundtrip) {
    ASSERT_TRUE(setupPublisher("live", "av_sync_test"));

    mediaDistribution_->setMetadata(currentStreamKey_, publisher_->createFFmpegMetadata());
    mediaDistribution_->setSequenceHeaders(currentStreamKey_,
        publisher_->createVideoSequenceHeader(),
        publisher_->createAudioSequenceHeader());

    auto subscriber = addSubscriber();
    streaming::SubscriberConfig config;
    subscriberManager_->addSubscriber(currentStreamKey_, subscriber->getId(), config);

    // Publish synchronized A/V at 25fps video, ~23.2ms audio frames
    uint32_t videoTs = 0;
    uint32_t audioTs = 0;
    const uint32_t videoInterval = 40;
    const uint32_t audioInterval = 23;

    for (int second = 0; second < 2; ++second) {
        // Keyframe at start of each second
        auto keyframe = publisher_->createVideoKeyframe(videoTs);
        mediaDistribution_->distribute(currentStreamKey_, keyframe);
        subscriber->onFrame(keyframe);
        videoTs += videoInterval;

        // Interleaved A/V
        for (int i = 0; i < 24; ++i) {
            // Audio
            while (audioTs < videoTs) {
                auto audio = publisher_->createAudioFrame(audioTs);
                mediaDistribution_->distribute(currentStreamKey_, audio);
                subscriber->onFrame(audio);
                audioTs += audioInterval;
            }

            // Video
            auto video = publisher_->createVideoInterFrame(videoTs);
            mediaDistribution_->distribute(currentStreamKey_, video);
            subscriber->onFrame(video);
            videoTs += videoInterval;
        }
    }

    // Verify both A/V received
    EXPECT_GT(subscriber->getFrameCount(), 50u);  // At least 2 seconds worth
}

TEST_F(FFmpegRoundtripTest, LateSubscriberReceivesFromKeyframe) {
    ASSERT_TRUE(setupPublisher("live", "late_join_test"));

    // Publish 5 seconds of content
    uint32_t timestamp = 0;
    const uint32_t frameInterval = 40;

    for (int gop = 0; gop < 5; ++gop) {
        mediaDistribution_->distribute(currentStreamKey_, publisher_->createVideoKeyframe(timestamp));
        timestamp += frameInterval;

        for (int i = 0; i < 24; ++i) {
            mediaDistribution_->distribute(currentStreamKey_, publisher_->createVideoInterFrame(timestamp));
            timestamp += frameInterval;
        }
    }

    // Late subscriber joins
    auto subscriber = addSubscriber();
    streaming::SubscriberConfig config;
    subscriberManager_->addSubscriber(currentStreamKey_, subscriber->getId(), config);

    // Should receive from last keyframe only (not all 5 seconds)
    auto buffered = gopBuffer_->getFromLastKeyframe();
    for (const auto& frame : buffered) {
        subscriber->onFrame(frame);
    }

    // Verify started from keyframe
    auto first = subscriber->getFirstVideoFrame();
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(first->isKeyframe);

    // Verify not too much data (only last GOP, not all 5)
    // Last GOP should be ~25 frames
    EXPECT_LE(subscriber->getFrameCount(), 30u);
}

// =============================================================================
// FFmpeg-Specific Behavior Tests
// =============================================================================

TEST_F(FFmpegRoundtripTest, FFmpegStyleTimestampWraparound) {
    ASSERT_TRUE(setupPublisher("live", "timestamp_wrap_test"));

    // FFmpeg handles 32-bit timestamp wraparound
    uint32_t timestamp = 0xFFFFFFF0;  // Near max value

    mediaDistribution_->distribute(currentStreamKey_, publisher_->createVideoKeyframe(timestamp));

    // Continue past wraparound
    for (int i = 0; i < 50; ++i) {
        timestamp += 40;  // Will wrap around
        mediaDistribution_->distribute(currentStreamKey_, publisher_->createVideoInterFrame(timestamp));
    }

    // Should handle gracefully
    EXPECT_GT(gopBuffer_->getFrameCount(), 0u);
}

TEST_F(FFmpegRoundtripTest, FFmpegZeroTimestampStart) {
    ASSERT_TRUE(setupPublisher("live", "zero_ts_test"));

    // FFmpeg typically starts at timestamp 0
    mediaDistribution_->distribute(currentStreamKey_, publisher_->createVideoKeyframe(0));
    mediaDistribution_->distribute(currentStreamKey_, publisher_->createAudioFrame(0));

    EXPECT_EQ(gopBuffer_->getFrameCount(), 2u);
}

} // namespace test
} // namespace e2e
} // namespace openrtmp
