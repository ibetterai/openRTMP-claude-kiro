// OpenRTMP - Cross-platform RTMP Server
// Integration Tests: Complete Publish Flow
//
// Task 21.2: Implement integration test suite
// Tests the complete publish flow from connect through media transmission
//
// Requirements coverage:
// - Requirement 4.1: Store stream with associated stream key
// - Requirement 4.2: Store video/audio codec information for new subscribers
// - Requirement 4.3: Send stream metadata to all current subscribers
// - Requirement 4.4: Store media frames for GOP buffer
// - Requirement 4.5: Forward media to distribution engine
// - Requirement 4.6: Maintain GOP buffer of at least 2 seconds
// - Requirement 4.7: Handle duplicate timestamps gracefully

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>

#include "openrtmp/streaming/stream_registry.hpp"
#include "openrtmp/streaming/gop_buffer.hpp"
#include "openrtmp/streaming/media_handler.hpp"
#include "openrtmp/streaming/subscriber_manager.hpp"
#include "openrtmp/streaming/media_distribution.hpp"
#include "openrtmp/protocol/handshake_handler.hpp"
#include "openrtmp/protocol/command_handler.hpp"
#include "openrtmp/protocol/amf_codec.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace integration {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class PublishFlowIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create the component hierarchy
        streamRegistry_ = std::make_shared<streaming::StreamRegistry>();
        subscriberManager_ = std::make_shared<streaming::SubscriberManager>(streamRegistry_);
        mediaDistribution_ = std::make_shared<streaming::MediaDistribution>(
            streamRegistry_, subscriberManager_);

        // Create AMF codec and command handler
        amfCodec_ = std::make_shared<protocol::AMFCodec>();
        commandHandler_ = std::make_unique<protocol::CommandHandler>(streamRegistry_, amfCodec_);

        // Initialize handshake handler
        handshakeHandler_ = std::make_unique<protocol::HandshakeHandler>();
    }

    void TearDown() override {
        // Clean up in reverse order
        handshakeHandler_.reset();
        commandHandler_.reset();
        mediaDistribution_.reset();
        subscriberManager_.reset();
        streamRegistry_->clear();
        streamRegistry_.reset();
    }

    // Helper to create a video keyframe
    streaming::BufferedFrame createVideoKeyframe(uint32_t timestamp, size_t dataSize = 1000) {
        streaming::BufferedFrame frame;
        frame.type = MediaType::Video;
        frame.timestamp = timestamp;
        frame.isKeyframe = true;
        frame.data.resize(dataSize);
        for (size_t i = 0; i < dataSize; ++i) {
            frame.data[i] = static_cast<uint8_t>((timestamp + i) & 0xFF);
        }
        return frame;
    }

    // Helper to create a video inter frame
    streaming::BufferedFrame createVideoInterFrame(uint32_t timestamp, size_t dataSize = 500) {
        streaming::BufferedFrame frame;
        frame.type = MediaType::Video;
        frame.timestamp = timestamp;
        frame.isKeyframe = false;
        frame.data.resize(dataSize);
        for (size_t i = 0; i < dataSize; ++i) {
            frame.data[i] = static_cast<uint8_t>((timestamp + i) & 0xFF);
        }
        return frame;
    }

    // Helper to create an audio frame
    streaming::BufferedFrame createAudioFrame(uint32_t timestamp, size_t dataSize = 200) {
        streaming::BufferedFrame frame;
        frame.type = MediaType::Audio;
        frame.timestamp = timestamp;
        frame.isKeyframe = true;  // Audio frames are always keyframes
        frame.data.resize(dataSize);
        for (size_t i = 0; i < dataSize; ++i) {
            frame.data[i] = static_cast<uint8_t>((timestamp + i) & 0xFF);
        }
        return frame;
    }

    // Helper to create test metadata
    protocol::AMFValue createTestMetadata() {
        std::map<std::string, protocol::AMFValue> metadata;
        metadata["width"] = protocol::AMFValue::makeNumber(1920);
        metadata["height"] = protocol::AMFValue::makeNumber(1080);
        metadata["framerate"] = protocol::AMFValue::makeNumber(30);
        metadata["videocodecid"] = protocol::AMFValue::makeString("avc1");
        metadata["audiocodecid"] = protocol::AMFValue::makeString("mp4a");
        return protocol::AMFValue::makeObject(std::move(metadata));
    }

    // Helper to simulate handshake
    bool simulateHandshake() {
        // C0: Version byte
        uint8_t c0 = 3;
        auto result = handshakeHandler_->processData(&c0, 1);
        if (!result.success) return false;

        // C1: 1536 bytes
        std::vector<uint8_t> c1(protocol::handshake::C1_SIZE);
        // Fill with timestamp and random data
        uint32_t timestamp = 0;
        c1[0] = (timestamp >> 24) & 0xFF;
        c1[1] = (timestamp >> 16) & 0xFF;
        c1[2] = (timestamp >> 8) & 0xFF;
        c1[3] = timestamp & 0xFF;
        // Zero bytes
        c1[4] = c1[5] = c1[6] = c1[7] = 0;
        // Random data
        for (size_t i = 8; i < c1.size(); ++i) {
            c1[i] = static_cast<uint8_t>(i & 0xFF);
        }

        result = handshakeHandler_->processData(c1.data(), c1.size());
        if (!result.success) return false;

        // Get S0+S1+S2 response
        auto response = handshakeHandler_->getResponseData();
        if (response.size() != protocol::handshake::S0_SIZE +
                              protocol::handshake::S1_SIZE +
                              protocol::handshake::S2_SIZE) {
            return false;
        }

        // C2: Echo S1
        std::vector<uint8_t> c2(protocol::handshake::C2_SIZE);
        // Copy S1 (skip S0)
        std::copy(response.begin() + protocol::handshake::S0_SIZE,
                  response.begin() + protocol::handshake::S0_SIZE + protocol::handshake::S1_SIZE,
                  c2.begin());

        result = handshakeHandler_->processData(c2.data(), c2.size());
        return result.success && handshakeHandler_->isComplete();
    }

    // Components
    std::shared_ptr<streaming::StreamRegistry> streamRegistry_;
    std::shared_ptr<streaming::SubscriberManager> subscriberManager_;
    std::shared_ptr<streaming::MediaDistribution> mediaDistribution_;
    std::shared_ptr<protocol::IAMFCodec> amfCodec_;
    std::unique_ptr<protocol::CommandHandler> commandHandler_;
    std::unique_ptr<protocol::HandshakeHandler> handshakeHandler_;
};

// =============================================================================
// Handshake Phase Tests
// =============================================================================

TEST_F(PublishFlowIntegrationTest, HandshakeCompletesSuccessfully) {
    ASSERT_TRUE(simulateHandshake());
    EXPECT_TRUE(handshakeHandler_->isComplete());
    EXPECT_EQ(handshakeHandler_->getState(), protocol::HandshakeState::Complete);
}

// =============================================================================
// Connect Command Tests
// =============================================================================

TEST_F(PublishFlowIntegrationTest, ConnectCommandEstablishesSession) {
    ASSERT_TRUE(simulateHandshake());

    // Create connect command
    protocol::RTMPCommand connectCmd;
    connectCmd.name = "connect";
    connectCmd.transactionId = 1.0;

    std::map<std::string, protocol::AMFValue> cmdObj;
    cmdObj["app"] = protocol::AMFValue::makeString("live");
    cmdObj["tcUrl"] = protocol::AMFValue::makeString("rtmp://localhost/live");
    cmdObj["flashVer"] = protocol::AMFValue::makeString("FMLE/3.0");
    connectCmd.commandObject = protocol::AMFValue::makeObject(std::move(cmdObj));

    protocol::SessionContext session;
    session.sessionId = 1;
    session.connectionId = 1;

    auto result = commandHandler_->handleConnect(connectCmd, session);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_TRUE(session.isConnected());
    EXPECT_EQ(session.appName, "live");
    EXPECT_EQ(session.state, protocol::SessionState::Connected);
}

// =============================================================================
// CreateStream Command Tests
// =============================================================================

TEST_F(PublishFlowIntegrationTest, CreateStreamAllocatesStreamId) {
    ASSERT_TRUE(simulateHandshake());

    // First connect
    protocol::RTMPCommand connectCmd;
    connectCmd.name = "connect";
    connectCmd.transactionId = 1.0;
    std::map<std::string, protocol::AMFValue> cmdObj;
    cmdObj["app"] = protocol::AMFValue::makeString("live");
    connectCmd.commandObject = protocol::AMFValue::makeObject(std::move(cmdObj));

    protocol::SessionContext session;
    session.sessionId = 1;
    session.connectionId = 1;
    commandHandler_->handleConnect(connectCmd, session);

    // Create stream
    protocol::RTMPCommand createStreamCmd;
    createStreamCmd.name = "createStream";
    createStreamCmd.transactionId = 2.0;
    createStreamCmd.commandObject = protocol::AMFValue::makeNull();

    auto result = commandHandler_->handleCreateStream(createStreamCmd, session);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_GT(session.streamId, 0u);
    EXPECT_TRUE(session.hasStream());
}

// =============================================================================
// Publish Command Tests
// =============================================================================

TEST_F(PublishFlowIntegrationTest, PublishCommandRegistersStream) {
    ASSERT_TRUE(simulateHandshake());

    // Setup session
    protocol::SessionContext session;
    session.sessionId = 1;
    session.connectionId = 1;
    session.state = protocol::SessionState::Connected;
    session.appName = "live";
    session.streamId = 1;

    // Register stream first in registry (as would happen in real flow)
    StreamKey streamKey{"live", "test_stream"};
    auto allocResult = streamRegistry_->allocateStreamId();
    ASSERT_TRUE(allocResult.isSuccess());

    // Publish command
    protocol::RTMPCommand publishCmd;
    publishCmd.name = "publish";
    publishCmd.transactionId = 3.0;
    publishCmd.commandObject = protocol::AMFValue::makeNull();
    publishCmd.args.push_back(protocol::AMFValue::makeString("test_stream"));
    publishCmd.args.push_back(protocol::AMFValue::makeString("live"));

    auto result = commandHandler_->handlePublish(publishCmd, session);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(session.state, protocol::SessionState::Publishing);
    EXPECT_EQ(session.streamKey, "test_stream");
}

TEST_F(PublishFlowIntegrationTest, StreamAppearsInRegistry) {
    // Direct registration test
    StreamKey streamKey{"live", "test_stream"};
    PublisherId publisherId = 1;

    auto allocResult = streamRegistry_->allocateStreamId();
    ASSERT_TRUE(allocResult.isSuccess());
    StreamId streamId = allocResult.value();

    auto regResult = streamRegistry_->registerStream(streamKey, streamId, publisherId);
    ASSERT_TRUE(regResult.isSuccess());

    // Verify stream is in registry
    EXPECT_TRUE(streamRegistry_->hasStream(streamKey));

    auto streamInfo = streamRegistry_->findStream(streamKey);
    ASSERT_TRUE(streamInfo.has_value());
    EXPECT_EQ(streamInfo->key, streamKey);
    EXPECT_EQ(streamInfo->streamId, streamId);
    EXPECT_EQ(streamInfo->publisherId, publisherId);
}

// =============================================================================
// Media Transmission Tests (Requirements 4.4, 4.5)
// =============================================================================

TEST_F(PublishFlowIntegrationTest, MediaBufferedInGOPBuffer) {
    // Setup stream
    StreamKey streamKey{"live", "test_stream"};
    auto allocResult = streamRegistry_->allocateStreamId();
    ASSERT_TRUE(allocResult.isSuccess());
    StreamId streamId = allocResult.value();
    streamRegistry_->registerStream(streamKey, streamId, 1);

    // Create and attach GOP buffer
    auto gopBuffer = std::make_shared<streaming::GOPBuffer>();
    mediaDistribution_->setGOPBuffer(streamKey, gopBuffer);

    // Send media frames
    auto keyframe = createVideoKeyframe(0);
    auto interframe1 = createVideoInterFrame(33);
    auto interframe2 = createVideoInterFrame(66);
    auto audio = createAudioFrame(0);

    mediaDistribution_->distribute(streamKey, keyframe);
    mediaDistribution_->distribute(streamKey, interframe1);
    mediaDistribution_->distribute(streamKey, interframe2);
    mediaDistribution_->distribute(streamKey, audio);

    // Verify frames are buffered
    EXPECT_GT(gopBuffer->getBufferedBytes(), 0u);
    EXPECT_GE(gopBuffer->getFrameCount(), 4u);
}

TEST_F(PublishFlowIntegrationTest, GOPBufferMaintains2SecondsMinimum) {
    // Create GOP buffer with default config
    auto gopBuffer = std::make_shared<streaming::GOPBuffer>();

    // Verify minimum buffer duration config
    auto config = gopBuffer->getConfig();
    EXPECT_GE(config.minBufferDuration.count(), 2000);

    // Setup stream
    StreamKey streamKey{"live", "test_stream"};
    auto allocResult = streamRegistry_->allocateStreamId();
    ASSERT_TRUE(allocResult.isSuccess());
    streamRegistry_->registerStream(streamKey, allocResult.value(), 1);
    mediaDistribution_->setGOPBuffer(streamKey, gopBuffer);

    // Simulate 4 seconds of video at 30fps
    uint32_t timestamp = 0;
    for (int gop = 0; gop < 4; ++gop) {
        // Keyframe starts each GOP (1 second each)
        mediaDistribution_->distribute(streamKey, createVideoKeyframe(timestamp));
        for (int frame = 1; frame < 30; ++frame) {
            timestamp += 33;
            mediaDistribution_->distribute(streamKey, createVideoInterFrame(timestamp));
        }
        timestamp += 33;
    }

    // Buffer should maintain at least 2 seconds
    auto duration = gopBuffer->getBufferedDuration();
    EXPECT_GE(duration.count(), 2000);
}

TEST_F(PublishFlowIntegrationTest, MetadataStoredSeparately) {
    // Setup stream with GOP buffer
    StreamKey streamKey{"live", "test_stream"};
    auto allocResult = streamRegistry_->allocateStreamId();
    ASSERT_TRUE(allocResult.isSuccess());
    streamRegistry_->registerStream(streamKey, allocResult.value(), 1);

    auto gopBuffer = std::make_shared<streaming::GOPBuffer>();
    mediaDistribution_->setGOPBuffer(streamKey, gopBuffer);

    // Set metadata
    auto metadata = createTestMetadata();
    mediaDistribution_->setMetadata(streamKey, metadata);

    // Verify metadata is stored
    auto storedMetadata = gopBuffer->getMetadata();
    ASSERT_TRUE(storedMetadata.has_value());

    // Verify metadata values
    auto& obj = storedMetadata->asObject();
    EXPECT_EQ(obj.at("width").asNumber(), 1920);
    EXPECT_EQ(obj.at("height").asNumber(), 1080);
}

TEST_F(PublishFlowIntegrationTest, SequenceHeadersStoredSeparately) {
    // Setup stream with GOP buffer
    StreamKey streamKey{"live", "test_stream"};
    auto allocResult = streamRegistry_->allocateStreamId();
    ASSERT_TRUE(allocResult.isSuccess());
    streamRegistry_->registerStream(streamKey, allocResult.value(), 1);

    auto gopBuffer = std::make_shared<streaming::GOPBuffer>();
    mediaDistribution_->setGOPBuffer(streamKey, gopBuffer);

    // Set sequence headers
    std::vector<uint8_t> videoHeader = {0x17, 0x00, 0x00, 0x00, 0x00}; // AVC sequence header
    std::vector<uint8_t> audioHeader = {0xAF, 0x00};  // AAC sequence header
    mediaDistribution_->setSequenceHeaders(streamKey, videoHeader, audioHeader);

    // Verify headers are stored
    auto headers = gopBuffer->getSequenceHeaders();
    EXPECT_EQ(headers.videoHeader, videoHeader);
    EXPECT_EQ(headers.audioHeader, audioHeader);
}

// =============================================================================
// Complete Publish Flow Test
// =============================================================================

TEST_F(PublishFlowIntegrationTest, CompletePublishFlowEndToEnd) {
    // Step 1: Handshake
    ASSERT_TRUE(simulateHandshake());
    EXPECT_TRUE(handshakeHandler_->isComplete());

    // Step 2: Connect
    protocol::RTMPCommand connectCmd;
    connectCmd.name = "connect";
    connectCmd.transactionId = 1.0;
    std::map<std::string, protocol::AMFValue> cmdObj;
    cmdObj["app"] = protocol::AMFValue::makeString("live");
    cmdObj["tcUrl"] = protocol::AMFValue::makeString("rtmp://localhost/live");
    connectCmd.commandObject = protocol::AMFValue::makeObject(std::move(cmdObj));

    protocol::SessionContext session;
    session.sessionId = 1;
    session.connectionId = 1;
    auto connectResult = commandHandler_->handleConnect(connectCmd, session);
    ASSERT_TRUE(connectResult.isSuccess());

    // Step 3: CreateStream
    protocol::RTMPCommand createStreamCmd;
    createStreamCmd.name = "createStream";
    createStreamCmd.transactionId = 2.0;
    createStreamCmd.commandObject = protocol::AMFValue::makeNull();
    auto createResult = commandHandler_->handleCreateStream(createStreamCmd, session);
    ASSERT_TRUE(createResult.isSuccess());

    // Step 4: Register stream in registry
    StreamKey streamKey{"live", "test_stream"};
    auto allocResult = streamRegistry_->allocateStreamId();
    ASSERT_TRUE(allocResult.isSuccess());
    StreamId streamId = allocResult.value();
    streamRegistry_->registerStream(streamKey, streamId, session.connectionId);

    // Step 5: Setup GOP buffer
    auto gopBuffer = std::make_shared<streaming::GOPBuffer>();
    mediaDistribution_->setGOPBuffer(streamKey, gopBuffer);

    // Step 6: Publish command
    protocol::RTMPCommand publishCmd;
    publishCmd.name = "publish";
    publishCmd.transactionId = 3.0;
    publishCmd.commandObject = protocol::AMFValue::makeNull();
    publishCmd.args.push_back(protocol::AMFValue::makeString("test_stream"));
    publishCmd.args.push_back(protocol::AMFValue::makeString("live"));
    auto publishResult = commandHandler_->handlePublish(publishCmd, session);
    ASSERT_TRUE(publishResult.isSuccess());

    // Step 7: Send metadata
    auto metadata = createTestMetadata();
    mediaDistribution_->setMetadata(streamKey, metadata);

    // Step 8: Send sequence headers
    std::vector<uint8_t> videoHeader = {0x17, 0x00, 0x00, 0x00, 0x00};
    std::vector<uint8_t> audioHeader = {0xAF, 0x00};
    mediaDistribution_->setSequenceHeaders(streamKey, videoHeader, audioHeader);

    // Step 9: Send media frames
    mediaDistribution_->distribute(streamKey, createVideoKeyframe(0));
    mediaDistribution_->distribute(streamKey, createAudioFrame(0));
    mediaDistribution_->distribute(streamKey, createVideoInterFrame(33));
    mediaDistribution_->distribute(streamKey, createVideoInterFrame(66));

    // Verify complete state
    EXPECT_TRUE(streamRegistry_->hasStream(streamKey));
    EXPECT_GT(gopBuffer->getBufferedBytes(), 0u);
    EXPECT_TRUE(gopBuffer->getMetadata().has_value());

    auto headers = gopBuffer->getSequenceHeaders();
    EXPECT_FALSE(headers.videoHeader.empty());
    EXPECT_FALSE(headers.audioHeader.empty());
}

// =============================================================================
// Duplicate Timestamp Handling Tests (Requirement 4.7)
// =============================================================================

TEST_F(PublishFlowIntegrationTest, HandlesDuplicateTimestamps) {
    StreamKey streamKey{"live", "test_stream"};
    auto allocResult = streamRegistry_->allocateStreamId();
    ASSERT_TRUE(allocResult.isSuccess());
    streamRegistry_->registerStream(streamKey, allocResult.value(), 1);

    auto gopBuffer = std::make_shared<streaming::GOPBuffer>();
    mediaDistribution_->setGOPBuffer(streamKey, gopBuffer);

    // Send frames with duplicate timestamps
    mediaDistribution_->distribute(streamKey, createVideoKeyframe(0));
    mediaDistribution_->distribute(streamKey, createAudioFrame(0));  // Same timestamp
    mediaDistribution_->distribute(streamKey, createVideoInterFrame(0));  // Same timestamp

    // Should handle gracefully - all frames should be stored
    EXPECT_GE(gopBuffer->getFrameCount(), 3u);
}

// =============================================================================
// Stream Cleanup Tests
// =============================================================================

TEST_F(PublishFlowIntegrationTest, StreamCleanupOnClose) {
    // Setup stream
    StreamKey streamKey{"live", "test_stream"};
    auto allocResult = streamRegistry_->allocateStreamId();
    ASSERT_TRUE(allocResult.isSuccess());
    StreamId streamId = allocResult.value();
    streamRegistry_->registerStream(streamKey, streamId, 1);

    EXPECT_TRUE(streamRegistry_->hasStream(streamKey));

    // Unregister stream
    auto unregResult = streamRegistry_->unregisterStream(streamKey);
    ASSERT_TRUE(unregResult.isSuccess());

    // Verify cleanup
    EXPECT_FALSE(streamRegistry_->hasStream(streamKey));
    EXPECT_FALSE(streamRegistry_->findStream(streamKey).has_value());
}

} // namespace test
} // namespace integration
} // namespace openrtmp
