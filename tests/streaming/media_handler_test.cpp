// OpenRTMP - Cross-platform RTMP Server
// Tests for Media Handler
//
// Tests cover:
// - Accept audio (type 8) and video (type 9) message types
// - Parse codec sequence headers for H.264/AVC and AAC
// - Detect Enhanced RTMP FourCC for H.265/HEVC codec support
// - Identify keyframes from NAL unit types for buffer management
// - Store stream metadata from data messages (type 18)
// - Forward validated media to GOP buffer and distribution
//
// Requirements coverage:
// - Requirement 4.1: Buffer and store stream with associated stream key
// - Requirement 4.2: Support H.264/AVC video codec
// - Requirement 4.3: Support AAC audio codec
// - Requirement 4.4: Accept Enhanced RTMP with HEVC/H.265 codec

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <cstdint>
#include <functional>

#include "openrtmp/streaming/media_handler.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace streaming {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class MediaHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        handler_ = std::make_unique<MediaHandler>();
    }

    void TearDown() override {
        handler_.reset();
    }

    std::unique_ptr<MediaHandler> handler_;

    // Helper to create a simple audio message
    MediaMessage createAudioMessage(uint32_t timestamp, const std::vector<uint8_t>& payload) {
        MediaMessage msg;
        msg.type = MediaType::Audio;
        msg.timestamp = timestamp;
        msg.payload = payload;
        msg.isKeyframe = true;  // Audio is always treated as keyframe for buffering
        return msg;
    }

    // Helper to create a simple video message
    MediaMessage createVideoMessage(uint32_t timestamp, const std::vector<uint8_t>& payload, bool isKeyframe = false) {
        MediaMessage msg;
        msg.type = MediaType::Video;
        msg.timestamp = timestamp;
        msg.payload = payload;
        msg.isKeyframe = isKeyframe;
        return msg;
    }

    // Helper to create a data message
    MediaMessage createDataMessage(uint32_t timestamp, const std::vector<uint8_t>& payload) {
        MediaMessage msg;
        msg.type = MediaType::Data;
        msg.timestamp = timestamp;
        msg.payload = payload;
        msg.isKeyframe = false;
        return msg;
    }

    // Helper to create AAC sequence header
    std::vector<uint8_t> createAACSequenceHeader() {
        // AAC sequence header format:
        // soundFormat[4] | soundRate[2] | soundSize[1] | soundType[1] | aacPacketType[8]
        // For AAC: soundFormat=10 (0xA), soundRate=3 (44kHz), soundSize=1 (16-bit), soundType=1 (stereo)
        // aacPacketType=0 for sequence header
        return {
            0xAF,  // AAC LC, 44kHz, 16-bit, stereo (1010 1111)
            0x00,  // AAC sequence header (aacPacketType=0)
            // AudioSpecificConfig (minimal)
            0x12, 0x10  // AAC LC, 44100Hz, 2 channels
        };
    }

    // Helper to create AAC raw data
    std::vector<uint8_t> createAACRawData() {
        return {
            0xAF,  // AAC LC, 44kHz, 16-bit, stereo
            0x01,  // AAC raw (aacPacketType=1)
            0x21, 0x00, 0x49, 0x90  // Sample AAC frame data
        };
    }

    // Helper to create H.264/AVC sequence header
    std::vector<uint8_t> createAVCSequenceHeader() {
        // AVC format:
        // frameType[4] | codecId[4] | avcPacketType[8] | compositionTime[24]
        // frameType=1 (keyframe), codecId=7 (AVC), avcPacketType=0 (sequence header)
        return {
            0x17,              // Keyframe + AVC (0001 0111)
            0x00,              // AVC sequence header
            0x00, 0x00, 0x00,  // Composition time = 0
            // AVCDecoderConfigurationRecord (minimal)
            0x01,              // configurationVersion
            0x42,              // AVCProfileIndication (Baseline)
            0x00,              // profile_compatibility
            0x1E,              // AVCLevelIndication (Level 3.0)
            0xFF,              // lengthSizeMinusOne (4 bytes NAL length)
            0xE1,              // numOfSequenceParameterSets (1)
            0x00, 0x0A,        // SPS length
            0x67, 0x42, 0x00, 0x1E, 0x96, 0x35, 0x40, 0xA0, 0x0C, 0xA0,  // SPS data
            0x01,              // numOfPictureParameterSets
            0x00, 0x04,        // PPS length
            0x68, 0xCE, 0x3C, 0x80  // PPS data
        };
    }

    // Helper to create AVC keyframe (IDR)
    std::vector<uint8_t> createAVCKeyframe() {
        return {
            0x17,              // Keyframe + AVC (0001 0111)
            0x01,              // AVC NALU
            0x00, 0x00, 0x00,  // Composition time = 0
            // NAL unit data (IDR slice, NAL type = 5)
            0x00, 0x00, 0x00, 0x08,  // NAL length (4 bytes)
            0x65, 0x88, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00  // IDR NAL unit
        };
    }

    // Helper to create AVC inter frame (P-frame)
    std::vector<uint8_t> createAVCInterFrame() {
        return {
            0x27,              // Inter frame + AVC (0010 0111)
            0x01,              // AVC NALU
            0x00, 0x00, 0x00,  // Composition time = 0
            // NAL unit data (non-IDR slice, NAL type = 1)
            0x00, 0x00, 0x00, 0x06,  // NAL length (4 bytes)
            0x41, 0x9A, 0x00, 0x00, 0x00, 0x00  // Non-IDR NAL unit
        };
    }

    // Helper to create Enhanced RTMP HEVC sequence header (FourCC "hvc1")
    std::vector<uint8_t> createHEVCSequenceHeader() {
        // Enhanced RTMP format:
        // frameType[4] | packetType[4] | fourCC[32]
        // packetType = 0b1000 (IsExHeader)
        return {
            0x90,                      // Keyframe + IsExHeader flag (1001 0000)
            'h', 'v', 'c', '1',        // FourCC "hvc1" for HEVC
            0x00,                      // PacketType = 0 (sequence start)
            // HEVCDecoderConfigurationRecord (minimal)
            0x01,                      // configurationVersion
            0x01,                      // general_profile_space, tier_flag, profile_idc
            0x60, 0x00, 0x00, 0x00,    // general_profile_compatibility_flags
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // general_constraint_indicator_flags
            0x5D,                      // general_level_idc (Level 3.1)
            0xF0, 0x00,                // min_spatial_segmentation_idc
            0xFC,                      // parallelismType
            0xFD,                      // chromaFormat
            0xF8,                      // bitDepthLumaMinus8
            0xF8,                      // bitDepthChromaMinus8
            0x00, 0x00,                // avgFrameRate
            0x0F,                      // constantFrameRate, numTemporalLayers, lengthSizeMinusOne
            0x01                       // numOfArrays (VPS, SPS, PPS would follow)
        };
    }

    // Helper to create Enhanced RTMP HEVC keyframe
    std::vector<uint8_t> createHEVCKeyframe() {
        return {
            0x90,                      // Keyframe + IsExHeader flag
            'h', 'v', 'c', '1',        // FourCC "hvc1"
            0x01,                      // PacketType = 1 (coded frames)
            0x00, 0x00, 0x00,          // Composition time
            // NAL unit (IDR_N_LP, NAL type = 20)
            0x00, 0x00, 0x00, 0x06,    // NAL length
            0x28, 0x01, 0x00, 0x00, 0x00, 0x00  // HEVC IDR NAL
        };
    }
};

// =============================================================================
// Media Type Acceptance Tests
// =============================================================================

TEST_F(MediaHandlerTest, AcceptsAudioMessageType8) {
    auto audioData = createAACRawData();
    auto msg = createAudioMessage(0, audioData);

    auto result = handler_->processMedia(msg);

    ASSERT_TRUE(result.isSuccess());
}

TEST_F(MediaHandlerTest, AcceptsVideoMessageType9) {
    auto videoData = createAVCKeyframe();
    auto msg = createVideoMessage(0, videoData, true);

    auto result = handler_->processMedia(msg);

    ASSERT_TRUE(result.isSuccess());
}

TEST_F(MediaHandlerTest, AcceptsDataMessageType18) {
    // Create a minimal @setDataFrame message
    std::vector<uint8_t> dataPayload = {0x02, 0x00, 0x0D}; // AMF0 string marker + length
    auto msg = createDataMessage(0, dataPayload);

    auto result = handler_->processMedia(msg);

    ASSERT_TRUE(result.isSuccess());
}

TEST_F(MediaHandlerTest, RejectsEmptyPayload) {
    MediaMessage msg;
    msg.type = MediaType::Video;
    msg.timestamp = 0;
    msg.payload = {};

    auto result = handler_->processMedia(msg);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, MediaHandlerError::Code::EmptyPayload);
}

// =============================================================================
// H.264/AVC Codec Tests
// =============================================================================

TEST_F(MediaHandlerTest, ParsesAVCSequenceHeader) {
    auto seqHeader = createAVCSequenceHeader();
    auto msg = createVideoMessage(0, seqHeader, true);

    auto result = handler_->processMedia(msg);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_TRUE(handler_->hasVideoSequenceHeader());
    EXPECT_EQ(handler_->getVideoCodec(), VideoCodec::H264);
}

TEST_F(MediaHandlerTest, IdentifiesAVCKeyframe) {
    // First send sequence header
    auto seqHeader = createAVCSequenceHeader();
    handler_->processMedia(createVideoMessage(0, seqHeader, true));

    // Then send keyframe
    auto keyframe = createAVCKeyframe();
    auto msg = createVideoMessage(100, keyframe);

    auto result = handler_->processMedia(msg);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_TRUE(handler_->isLastFrameKeyframe());
}

TEST_F(MediaHandlerTest, IdentifiesAVCInterFrame) {
    // First send sequence header
    auto seqHeader = createAVCSequenceHeader();
    handler_->processMedia(createVideoMessage(0, seqHeader, true));

    // Then send inter frame
    auto interFrame = createAVCInterFrame();
    auto msg = createVideoMessage(100, interFrame);

    auto result = handler_->processMedia(msg);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_FALSE(handler_->isLastFrameKeyframe());
}

TEST_F(MediaHandlerTest, ParsesAVCNALUnitTypes) {
    // First send sequence header
    auto seqHeader = createAVCSequenceHeader();
    handler_->processMedia(createVideoMessage(0, seqHeader, true));

    // Send keyframe (IDR, NAL type 5)
    auto keyframe = createAVCKeyframe();
    handler_->processMedia(createVideoMessage(100, keyframe));

    EXPECT_EQ(handler_->getLastNALUnitType(), 5u);  // IDR slice

    // Send inter frame (non-IDR, NAL type 1)
    auto interFrame = createAVCInterFrame();
    handler_->processMedia(createVideoMessage(200, interFrame));

    EXPECT_EQ(handler_->getLastNALUnitType(), 1u);  // Non-IDR slice
}

// =============================================================================
// AAC Audio Codec Tests
// =============================================================================

TEST_F(MediaHandlerTest, ParsesAACSequenceHeader) {
    auto seqHeader = createAACSequenceHeader();
    auto msg = createAudioMessage(0, seqHeader);

    auto result = handler_->processMedia(msg);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_TRUE(handler_->hasAudioSequenceHeader());
    EXPECT_EQ(handler_->getAudioCodec(), AudioCodec::AAC);
}

TEST_F(MediaHandlerTest, ParsesAACRawData) {
    // First send sequence header
    auto seqHeader = createAACSequenceHeader();
    handler_->processMedia(createAudioMessage(0, seqHeader));

    // Then send raw AAC data
    auto rawData = createAACRawData();
    auto msg = createAudioMessage(100, rawData);

    auto result = handler_->processMedia(msg);

    ASSERT_TRUE(result.isSuccess());
}

TEST_F(MediaHandlerTest, DetectsAudioSampleRate) {
    auto seqHeader = createAACSequenceHeader();
    handler_->processMedia(createAudioMessage(0, seqHeader));

    EXPECT_EQ(handler_->getAudioSampleRate(), 44100u);
}

TEST_F(MediaHandlerTest, DetectsAudioChannels) {
    auto seqHeader = createAACSequenceHeader();
    handler_->processMedia(createAudioMessage(0, seqHeader));

    EXPECT_EQ(handler_->getAudioChannels(), 2u);  // Stereo
}

// =============================================================================
// Enhanced RTMP H.265/HEVC Tests
// =============================================================================

TEST_F(MediaHandlerTest, DetectsEnhancedRTMPFlag) {
    auto hevcSeqHeader = createHEVCSequenceHeader();
    auto msg = createVideoMessage(0, hevcSeqHeader, true);

    auto result = handler_->processMedia(msg);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_TRUE(handler_->isEnhancedRTMP());
}

TEST_F(MediaHandlerTest, ParsesHEVCFourCC) {
    auto hevcSeqHeader = createHEVCSequenceHeader();
    auto msg = createVideoMessage(0, hevcSeqHeader, true);

    handler_->processMedia(msg);

    EXPECT_EQ(handler_->getVideoCodec(), VideoCodec::H265);
    EXPECT_EQ(handler_->getFourCC(), "hvc1");
}

TEST_F(MediaHandlerTest, ParsesHEVCSequenceHeader) {
    auto hevcSeqHeader = createHEVCSequenceHeader();
    auto msg = createVideoMessage(0, hevcSeqHeader, true);

    auto result = handler_->processMedia(msg);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_TRUE(handler_->hasVideoSequenceHeader());
}

TEST_F(MediaHandlerTest, IdentifiesHEVCKeyframe) {
    // First send sequence header
    auto seqHeader = createHEVCSequenceHeader();
    handler_->processMedia(createVideoMessage(0, seqHeader, true));

    // Then send keyframe
    auto keyframe = createHEVCKeyframe();
    auto msg = createVideoMessage(100, keyframe);

    auto result = handler_->processMedia(msg);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_TRUE(handler_->isLastFrameKeyframe());
}

// =============================================================================
// Keyframe Detection Tests (for Buffer Management)
// =============================================================================

TEST_F(MediaHandlerTest, KeyframeDetectionFromFrameType) {
    // AVC keyframe has frame type = 1 in upper nibble
    auto keyframe = createAVCKeyframe();
    auto msg = createVideoMessage(0, keyframe);

    handler_->processMedia(msg);

    EXPECT_TRUE(handler_->isLastFrameKeyframe());
}

TEST_F(MediaHandlerTest, InterFrameDetectionFromFrameType) {
    // AVC inter frame has frame type = 2 in upper nibble
    auto interFrame = createAVCInterFrame();
    auto msg = createVideoMessage(0, interFrame);

    handler_->processMedia(msg);

    EXPECT_FALSE(handler_->isLastFrameKeyframe());
}

TEST_F(MediaHandlerTest, KeyframeCallbackIsInvoked) {
    bool callbackInvoked = false;
    uint32_t keyframeTimestamp = 0;

    handler_->setKeyframeCallback([&](uint32_t timestamp) {
        callbackInvoked = true;
        keyframeTimestamp = timestamp;
    });

    // Send sequence header
    auto seqHeader = createAVCSequenceHeader();
    handler_->processMedia(createVideoMessage(0, seqHeader, true));

    // Send keyframe
    auto keyframe = createAVCKeyframe();
    handler_->processMedia(createVideoMessage(12345, keyframe));

    EXPECT_TRUE(callbackInvoked);
    EXPECT_EQ(keyframeTimestamp, 12345u);
}

// =============================================================================
// Stream Metadata Tests (Type 18 Data Messages)
// =============================================================================

TEST_F(MediaHandlerTest, StoresStreamMetadata) {
    // Create an @setDataFrame onMetaData message (simplified)
    // In real RTMP, this would be AMF-encoded
    std::vector<uint8_t> metadata = {
        0x02, 0x00, 0x0D,  // AMF0 string marker + "onMetaData" length
        'o', 'n', 'M', 'e', 't', 'a', 'D', 'a', 't', 'a', 0x00, 0x00, 0x00
    };

    auto msg = createDataMessage(0, metadata);

    auto result = handler_->processMedia(msg);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_TRUE(handler_->hasMetadata());
}

TEST_F(MediaHandlerTest, MetadataCallbackIsInvoked) {
    bool callbackInvoked = false;

    handler_->setMetadataCallback([&](const std::vector<uint8_t>& data) {
        callbackInvoked = true;
    });

    std::vector<uint8_t> metadata = {0x02, 0x00, 0x0A};
    auto msg = createDataMessage(0, metadata);

    handler_->processMedia(msg);

    EXPECT_TRUE(callbackInvoked);
}

// =============================================================================
// Media Forwarding Tests
// =============================================================================

TEST_F(MediaHandlerTest, ForwardsMediaViaCallback) {
    std::vector<MediaMessage> forwardedMessages;

    handler_->setMediaForwardCallback([&](const MediaMessage& msg) {
        forwardedMessages.push_back(msg);
    });

    // Send sequence header
    auto seqHeader = createAVCSequenceHeader();
    handler_->processMedia(createVideoMessage(0, seqHeader, true));

    // Send keyframe
    auto keyframe = createAVCKeyframe();
    handler_->processMedia(createVideoMessage(100, keyframe));

    // Send inter frame
    auto interFrame = createAVCInterFrame();
    handler_->processMedia(createVideoMessage(200, interFrame));

    EXPECT_EQ(forwardedMessages.size(), 3u);
}

TEST_F(MediaHandlerTest, ValidatesMediaBeforeForwarding) {
    std::vector<MediaMessage> forwardedMessages;

    handler_->setMediaForwardCallback([&](const MediaMessage& msg) {
        forwardedMessages.push_back(msg);
    });

    // Send invalid empty payload
    MediaMessage emptyMsg;
    emptyMsg.type = MediaType::Video;
    emptyMsg.timestamp = 0;
    emptyMsg.payload = {};

    handler_->processMedia(emptyMsg);

    // Invalid message should not be forwarded
    EXPECT_EQ(forwardedMessages.size(), 0u);
}

// =============================================================================
// Codec Info Extraction Tests
// =============================================================================

TEST_F(MediaHandlerTest, ExtractsCompleteCodecInfo) {
    // Send video sequence header
    auto videoSeqHeader = createAVCSequenceHeader();
    handler_->processMedia(createVideoMessage(0, videoSeqHeader, true));

    // Send audio sequence header
    auto audioSeqHeader = createAACSequenceHeader();
    handler_->processMedia(createAudioMessage(0, audioSeqHeader));

    auto codecInfo = handler_->getCodecInfo();

    EXPECT_EQ(codecInfo.videoCodec, VideoCodec::H264);
    EXPECT_EQ(codecInfo.audioCodec, AudioCodec::AAC);
}

TEST_F(MediaHandlerTest, ReturnsUnknownCodecsWhenNotInitialized) {
    auto codecInfo = handler_->getCodecInfo();

    EXPECT_EQ(codecInfo.videoCodec, VideoCodec::Unknown);
    EXPECT_EQ(codecInfo.audioCodec, AudioCodec::Unknown);
}

// =============================================================================
// Sequence Header Caching Tests
// =============================================================================

TEST_F(MediaHandlerTest, CachesVideoSequenceHeader) {
    auto seqHeader = createAVCSequenceHeader();
    handler_->processMedia(createVideoMessage(0, seqHeader, true));

    auto cachedHeader = handler_->getVideoSequenceHeader();

    EXPECT_FALSE(cachedHeader.empty());
    EXPECT_EQ(cachedHeader, seqHeader);
}

TEST_F(MediaHandlerTest, CachesAudioSequenceHeader) {
    auto seqHeader = createAACSequenceHeader();
    handler_->processMedia(createAudioMessage(0, seqHeader));

    auto cachedHeader = handler_->getAudioSequenceHeader();

    EXPECT_FALSE(cachedHeader.empty());
    EXPECT_EQ(cachedHeader, seqHeader);
}

TEST_F(MediaHandlerTest, UpdatesSequenceHeaderOnNewOne) {
    // First sequence header
    auto seqHeader1 = createAVCSequenceHeader();
    handler_->processMedia(createVideoMessage(0, seqHeader1, true));

    // Send a different sequence header (simulate codec change)
    auto seqHeader2 = createHEVCSequenceHeader();
    handler_->processMedia(createVideoMessage(1000, seqHeader2, true));

    // Should have the new header
    EXPECT_EQ(handler_->getVideoCodec(), VideoCodec::H265);
}

// =============================================================================
// Reset and Clear Tests
// =============================================================================

TEST_F(MediaHandlerTest, ResetClearsAllState) {
    // Setup state
    auto videoSeqHeader = createAVCSequenceHeader();
    handler_->processMedia(createVideoMessage(0, videoSeqHeader, true));

    auto audioSeqHeader = createAACSequenceHeader();
    handler_->processMedia(createAudioMessage(0, audioSeqHeader));

    // Reset
    handler_->reset();

    // Verify cleared
    EXPECT_FALSE(handler_->hasVideoSequenceHeader());
    EXPECT_FALSE(handler_->hasAudioSequenceHeader());
    EXPECT_FALSE(handler_->hasMetadata());
    EXPECT_EQ(handler_->getVideoCodec(), VideoCodec::Unknown);
    EXPECT_EQ(handler_->getAudioCodec(), AudioCodec::Unknown);
}

} // namespace test
} // namespace streaming
} // namespace openrtmp
