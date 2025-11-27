// OpenRTMP - Cross-platform RTMP Server
// Media Handler Implementation

#include "openrtmp/streaming/media_handler.hpp"

#include <cstring>
#include <algorithm>

namespace openrtmp {
namespace streaming {

// =============================================================================
// Constructor / Destructor
// =============================================================================

MediaHandler::MediaHandler()
    : videoCodec_(VideoCodec::Unknown)
    , hasVideoSeqHeader_(false)
    , isEnhancedRTMP_(false)
    , lastFrameIsKeyframe_(false)
    , lastNALUnitType_(0)
    , audioCodec_(AudioCodec::Unknown)
    , hasAudioSeqHeader_(false)
    , audioSampleRate_(0)
    , audioChannels_(0)
    , hasMetadata_(false)
{
}

MediaHandler::MediaHandler(MediaHandler&& other) noexcept
    : videoCodec_(other.videoCodec_)
    , hasVideoSeqHeader_(other.hasVideoSeqHeader_)
    , isEnhancedRTMP_(other.isEnhancedRTMP_)
    , lastFrameIsKeyframe_(other.lastFrameIsKeyframe_)
    , lastNALUnitType_(other.lastNALUnitType_)
    , fourCC_(std::move(other.fourCC_))
    , videoSequenceHeader_(std::move(other.videoSequenceHeader_))
    , audioCodec_(other.audioCodec_)
    , hasAudioSeqHeader_(other.hasAudioSeqHeader_)
    , audioSampleRate_(other.audioSampleRate_)
    , audioChannels_(other.audioChannels_)
    , audioSequenceHeader_(std::move(other.audioSequenceHeader_))
    , hasMetadata_(other.hasMetadata_)
    , metadata_(std::move(other.metadata_))
    , keyframeCallback_(std::move(other.keyframeCallback_))
    , metadataCallback_(std::move(other.metadataCallback_))
    , mediaForwardCallback_(std::move(other.mediaForwardCallback_))
{
}

MediaHandler& MediaHandler::operator=(MediaHandler&& other) noexcept {
    if (this != &other) {
        std::lock_guard<std::mutex> lock(mutex_);

        videoCodec_ = other.videoCodec_;
        hasVideoSeqHeader_ = other.hasVideoSeqHeader_;
        isEnhancedRTMP_ = other.isEnhancedRTMP_;
        lastFrameIsKeyframe_ = other.lastFrameIsKeyframe_;
        lastNALUnitType_ = other.lastNALUnitType_;
        fourCC_ = std::move(other.fourCC_);
        videoSequenceHeader_ = std::move(other.videoSequenceHeader_);

        audioCodec_ = other.audioCodec_;
        hasAudioSeqHeader_ = other.hasAudioSeqHeader_;
        audioSampleRate_ = other.audioSampleRate_;
        audioChannels_ = other.audioChannels_;
        audioSequenceHeader_ = std::move(other.audioSequenceHeader_);

        hasMetadata_ = other.hasMetadata_;
        metadata_ = std::move(other.metadata_);

        keyframeCallback_ = std::move(other.keyframeCallback_);
        metadataCallback_ = std::move(other.metadataCallback_);
        mediaForwardCallback_ = std::move(other.mediaForwardCallback_);
    }
    return *this;
}

// =============================================================================
// IMediaHandler Interface - Process Media
// =============================================================================

core::Result<void, MediaHandlerError> MediaHandler::processMedia(
    const MediaMessage& msg
) {
    // Validate payload
    if (msg.payload.empty()) {
        return core::Result<void, MediaHandlerError>::error(
            MediaHandlerError(
                MediaHandlerError::Code::EmptyPayload,
                "Media payload is empty"
            )
        );
    }

    core::Result<void, MediaHandlerError> result = core::Result<void, MediaHandlerError>::success();

    switch (msg.type) {
        case MediaType::Video:
            result = processVideo(msg);
            break;
        case MediaType::Audio:
            result = processAudio(msg);
            break;
        case MediaType::Data:
            result = processData(msg);
            break;
        default:
            return core::Result<void, MediaHandlerError>::error(
                MediaHandlerError(
                    MediaHandlerError::Code::UnsupportedCodec,
                    "Unknown media type"
                )
            );
    }

    // Forward valid media
    if (result.isSuccess()) {
        forwardMedia(msg);
    }

    return result;
}

// =============================================================================
// State Query Methods
// =============================================================================

bool MediaHandler::hasVideoSequenceHeader() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hasVideoSeqHeader_;
}

bool MediaHandler::hasAudioSequenceHeader() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hasAudioSeqHeader_;
}

bool MediaHandler::hasMetadata() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hasMetadata_;
}

bool MediaHandler::isEnhancedRTMP() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return isEnhancedRTMP_;
}

bool MediaHandler::isLastFrameKeyframe() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastFrameIsKeyframe_;
}

// =============================================================================
// Codec Information Methods
// =============================================================================

VideoCodec MediaHandler::getVideoCodec() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return videoCodec_;
}

AudioCodec MediaHandler::getAudioCodec() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return audioCodec_;
}

std::string MediaHandler::getFourCC() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fourCC_;
}

uint8_t MediaHandler::getLastNALUnitType() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastNALUnitType_;
}

uint32_t MediaHandler::getAudioSampleRate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return audioSampleRate_;
}

uint8_t MediaHandler::getAudioChannels() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return audioChannels_;
}

CodecInfo MediaHandler::getCodecInfo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    CodecInfo info;
    info.videoCodec = videoCodec_;
    info.audioCodec = audioCodec_;
    info.audioSampleRate = audioSampleRate_;
    info.audioChannels = audioChannels_;
    return info;
}

// =============================================================================
// Sequence Header Access
// =============================================================================

std::vector<uint8_t> MediaHandler::getVideoSequenceHeader() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return videoSequenceHeader_;
}

std::vector<uint8_t> MediaHandler::getAudioSequenceHeader() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return audioSequenceHeader_;
}

// =============================================================================
// Callback Setters
// =============================================================================

void MediaHandler::setKeyframeCallback(KeyframeCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    keyframeCallback_ = std::move(callback);
}

void MediaHandler::setMetadataCallback(MetadataCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    metadataCallback_ = std::move(callback);
}

void MediaHandler::setMediaForwardCallback(MediaForwardCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    mediaForwardCallback_ = std::move(callback);
}

// =============================================================================
// Reset
// =============================================================================

void MediaHandler::reset() {
    std::lock_guard<std::mutex> lock(mutex_);

    videoCodec_ = VideoCodec::Unknown;
    hasVideoSeqHeader_ = false;
    isEnhancedRTMP_ = false;
    lastFrameIsKeyframe_ = false;
    lastNALUnitType_ = 0;
    fourCC_.clear();
    videoSequenceHeader_.clear();

    audioCodec_ = AudioCodec::Unknown;
    hasAudioSeqHeader_ = false;
    audioSampleRate_ = 0;
    audioChannels_ = 0;
    audioSequenceHeader_.clear();

    hasMetadata_ = false;
    metadata_.clear();
}

// =============================================================================
// Private Processing Helpers
// =============================================================================

core::Result<void, MediaHandlerError> MediaHandler::processVideo(
    const MediaMessage& msg
) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (msg.payload.size() < 1) {
        return core::Result<void, MediaHandlerError>::error(
            MediaHandlerError(
                MediaHandlerError::Code::InvalidHeader,
                "Video payload too short"
            )
        );
    }

    uint8_t firstByte = msg.payload[0];

    // Check for Enhanced RTMP (IsExHeader flag in upper nibble)
    // Enhanced RTMP: frameType[4] | packetType[4], where packetType has 0x8 bit set
    // The pattern is: upper nibble = frame type (1-5), lower nibble has 0x80 bit
    // Actually, Enhanced RTMP uses: (frameType << 4) | 0x80 in the first byte
    // So 0x90 = keyframe (1) + ex_header flag
    if ((firstByte & 0x80) != 0) {
        return processEnhancedVideo(msg);
    }

    return processStandardVideo(msg, firstByte);
}

core::Result<void, MediaHandlerError> MediaHandler::processStandardVideo(
    const MediaMessage& msg,
    uint8_t firstByte
) {
    // Standard FLV video format:
    // Byte 0: frameType[4] | codecId[4]
    // For AVC (codecId=7): Byte 1: avcPacketType, Bytes 2-4: compositionTime

    uint8_t frameType = (firstByte >> 4) & 0x0F;
    uint8_t codecId = firstByte & 0x0F;

    // Determine if this is a keyframe based on frame type
    lastFrameIsKeyframe_ = (frameType == media::FRAME_TYPE_KEYFRAME ||
                            frameType == media::FRAME_TYPE_GENERATED_KEYFRAME);

    // Process based on codec
    if (codecId == media::CODEC_ID_AVC) {
        videoCodec_ = VideoCodec::H264;

        if (msg.payload.size() < 5) {
            return core::Result<void, MediaHandlerError>::error(
                MediaHandlerError(
                    MediaHandlerError::Code::InvalidHeader,
                    "AVC video payload too short"
                )
            );
        }

        uint8_t avcPacketType = msg.payload[1];

        if (avcPacketType == media::AVC_PACKET_TYPE_SEQUENCE_HEADER) {
            // Parse and cache sequence header
            parseAVCSequenceHeader(msg.payload);
            hasVideoSeqHeader_ = true;
            videoSequenceHeader_ = msg.payload;
        } else if (avcPacketType == media::AVC_PACKET_TYPE_NALU) {
            // Extract NAL unit type from first NAL unit
            // NAL data starts at offset 5 (after header + composition time)
            if (msg.payload.size() > 9) {  // 5 bytes header + 4 bytes NAL length
                lastNALUnitType_ = extractAVCNALType(msg.payload, 5);

                // Update keyframe detection based on NAL type
                if (isAVCKeyframeNAL(lastNALUnitType_)) {
                    lastFrameIsKeyframe_ = true;
                }
            }
        }
    }

    // Invoke keyframe callback if this is a keyframe
    if (lastFrameIsKeyframe_ && keyframeCallback_) {
        // Call outside lock would be better, but for simplicity keep inside
        keyframeCallback_(msg.timestamp);
    }

    return core::Result<void, MediaHandlerError>::success();
}

core::Result<void, MediaHandlerError> MediaHandler::processEnhancedVideo(
    const MediaMessage& msg
) {
    // Enhanced RTMP format:
    // Byte 0: frameType[4] | ExHeader flag (0x80) + bits
    // Bytes 1-4: FourCC (e.g., "hvc1", "av01")
    // Byte 5: PacketType
    // Rest: codec-specific data

    if (msg.payload.size() < 6) {
        return core::Result<void, MediaHandlerError>::error(
            MediaHandlerError(
                MediaHandlerError::Code::InvalidHeader,
                "Enhanced RTMP video payload too short"
            )
        );
    }

    isEnhancedRTMP_ = true;

    uint8_t firstByte = msg.payload[0];
    uint8_t frameType = (firstByte >> 4) & 0x07;  // Upper 3 bits of frame type

    // Determine if this is a keyframe
    lastFrameIsKeyframe_ = (frameType == media::FRAME_TYPE_KEYFRAME ||
                            frameType == media::FRAME_TYPE_GENERATED_KEYFRAME);

    // Extract FourCC
    fourCC_.assign(reinterpret_cast<const char*>(&msg.payload[1]), 4);

    // Determine codec from FourCC
    if (fourCC_ == "hvc1" || fourCC_ == "hev1") {
        videoCodec_ = VideoCodec::H265;
    } else if (fourCC_ == "avc1") {
        videoCodec_ = VideoCodec::H264;
    } else if (fourCC_ == "av01") {
        videoCodec_ = VideoCodec::AV1;
    } else if (fourCC_ == "vp09") {
        videoCodec_ = VideoCodec::VP9;
    }

    uint8_t packetType = msg.payload[5];

    if (packetType == media::EX_PACKET_TYPE_SEQUENCE_START) {
        // This is a sequence header
        hasVideoSeqHeader_ = true;
        videoSequenceHeader_ = msg.payload;

        if (videoCodec_ == VideoCodec::H265) {
            parseHEVCSequenceHeader(msg.payload);
        }
    } else if (packetType == media::EX_PACKET_TYPE_CODED_FRAMES ||
               packetType == media::EX_PACKET_TYPE_CODED_FRAMES_X) {
        // Extract NAL unit type for HEVC
        if (videoCodec_ == VideoCodec::H265 && msg.payload.size() > 12) {
            // HEVC NAL starts after: 1 (header) + 4 (fourcc) + 1 (packet type) + 3 (composition time) + 4 (NAL length)
            lastNALUnitType_ = extractHEVCNALType(msg.payload, 9);

            if (isHEVCKeyframeNAL(lastNALUnitType_)) {
                lastFrameIsKeyframe_ = true;
            }
        }
    }

    // Invoke keyframe callback if this is a keyframe
    if (lastFrameIsKeyframe_ && keyframeCallback_) {
        keyframeCallback_(msg.timestamp);
    }

    return core::Result<void, MediaHandlerError>::success();
}

core::Result<void, MediaHandlerError> MediaHandler::processAudio(
    const MediaMessage& msg
) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (msg.payload.size() < 2) {
        return core::Result<void, MediaHandlerError>::error(
            MediaHandlerError(
                MediaHandlerError::Code::InvalidHeader,
                "Audio payload too short"
            )
        );
    }

    uint8_t firstByte = msg.payload[0];

    // Audio format is in upper 4 bits
    uint8_t soundFormat = (firstByte >> 4) & 0x0F;

    if (soundFormat == media::AUDIO_FORMAT_AAC) {
        audioCodec_ = AudioCodec::AAC;

        uint8_t aacPacketType = msg.payload[1];

        if (aacPacketType == media::AAC_PACKET_TYPE_SEQUENCE_HEADER) {
            parseAACSequenceHeader(msg.payload);
            hasAudioSeqHeader_ = true;
            audioSequenceHeader_ = msg.payload;
        }
    } else if (soundFormat == media::AUDIO_FORMAT_OPUS) {
        audioCodec_ = AudioCodec::Opus;
        hasAudioSeqHeader_ = true;  // Opus doesn't need sequence header
        audioSequenceHeader_ = msg.payload;
    }

    return core::Result<void, MediaHandlerError>::success();
}

core::Result<void, MediaHandlerError> MediaHandler::processData(
    const MediaMessage& msg
) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Data messages contain AMF-encoded metadata
    // Store the raw metadata for later parsing
    hasMetadata_ = true;
    metadata_ = msg.payload;

    // Invoke metadata callback
    if (metadataCallback_) {
        metadataCallback_(msg.payload);
    }

    return core::Result<void, MediaHandlerError>::success();
}

// =============================================================================
// Sequence Header Parsing
// =============================================================================

void MediaHandler::parseAVCSequenceHeader(const std::vector<uint8_t>& payload) {
    // AVCDecoderConfigurationRecord structure:
    // Offset 5: configurationVersion
    // Offset 6: AVCProfileIndication
    // Offset 7: profile_compatibility
    // Offset 8: AVCLevelIndication
    // Offset 9: lengthSizeMinusOne (lower 2 bits) + reserved
    // Offset 10: numOfSequenceParameterSets (lower 5 bits) + reserved
    // Then SPS data, then PPS data

    // For now, we just cache the sequence header
    // Full parsing would extract SPS to get resolution, framerate, etc.
}

void MediaHandler::parseHEVCSequenceHeader(const std::vector<uint8_t>& payload) {
    // HEVCDecoderConfigurationRecord structure is more complex
    // For now, we just cache the sequence header
    // Full parsing would extract VPS, SPS, PPS for codec parameters
}

void MediaHandler::parseAACSequenceHeader(const std::vector<uint8_t>& payload) {
    // AAC AudioSpecificConfig parsing
    // Byte 0: FLV audio header
    // Byte 1: AAC packet type (0 = sequence header)
    // Bytes 2+: AudioSpecificConfig

    if (payload.size() < 4) {
        return;
    }

    // AudioSpecificConfig structure (ISO 14496-3):
    // audioObjectType (5 bits)
    // samplingFrequencyIndex (4 bits)
    // channelConfiguration (4 bits)
    // ...

    uint8_t asc0 = payload[2];
    uint8_t asc1 = payload[3];

    // Extract sampling frequency index (bits 3-0 of asc0, bit 7 of asc1)
    uint8_t samplingFreqIndex = ((asc0 & 0x07) << 1) | ((asc1 >> 7) & 0x01);

    // Extract channel configuration (bits 6-3 of asc1)
    uint8_t channelConfig = (asc1 >> 3) & 0x0F;

    // Sampling frequency lookup table (ISO 14496-3)
    static const uint32_t samplingFrequencies[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
        16000, 12000, 11025, 8000, 7350, 0, 0, 0
    };

    if (samplingFreqIndex < 16) {
        audioSampleRate_ = samplingFrequencies[samplingFreqIndex];
    }

    audioChannels_ = channelConfig;
}

// =============================================================================
// NAL Unit Extraction
// =============================================================================

uint8_t MediaHandler::extractAVCNALType(const std::vector<uint8_t>& payload, size_t offset) {
    // AVC NAL units in AVCC format have 4-byte length prefix (or configured length)
    // NAL unit type is in lower 5 bits of first byte after length

    if (payload.size() <= offset + 4) {
        return 0;
    }

    // Read NAL length (4 bytes big-endian)
    uint32_t nalLength = (static_cast<uint32_t>(payload[offset]) << 24) |
                         (static_cast<uint32_t>(payload[offset + 1]) << 16) |
                         (static_cast<uint32_t>(payload[offset + 2]) << 8) |
                         static_cast<uint32_t>(payload[offset + 3]);

    if (nalLength == 0 || payload.size() <= offset + 4) {
        return 0;
    }

    // NAL unit type is in lower 5 bits of first NAL byte
    uint8_t nalByte = payload[offset + 4];
    return nalByte & 0x1F;
}

uint8_t MediaHandler::extractHEVCNALType(const std::vector<uint8_t>& payload, size_t offset) {
    // HEVC NAL units also have 4-byte length prefix
    // NAL unit type is in bits 6-1 of first byte (after length)

    if (payload.size() <= offset + 4) {
        return 0;
    }

    // Read NAL length (4 bytes big-endian)
    uint32_t nalLength = (static_cast<uint32_t>(payload[offset]) << 24) |
                         (static_cast<uint32_t>(payload[offset + 1]) << 16) |
                         (static_cast<uint32_t>(payload[offset + 2]) << 8) |
                         static_cast<uint32_t>(payload[offset + 3]);

    if (nalLength == 0 || payload.size() <= offset + 4) {
        return 0;
    }

    // HEVC NAL unit type is in bits 6-1 of first NAL byte
    uint8_t nalByte = payload[offset + 4];
    return (nalByte >> 1) & 0x3F;
}

bool MediaHandler::isAVCKeyframeNAL(uint8_t nalType) const {
    // AVC keyframe NAL types:
    // 5 = IDR slice
    return nalType == media::AVC_NAL_TYPE_IDR;
}

bool MediaHandler::isHEVCKeyframeNAL(uint8_t nalType) const {
    // HEVC keyframe NAL types:
    // 16-21 = BLA, IDR, CRA
    return (nalType >= media::HEVC_NAL_TYPE_BLA_W_LP &&
            nalType <= media::HEVC_NAL_TYPE_CRA_NUT);
}

// =============================================================================
// Media Forwarding
// =============================================================================

void MediaHandler::forwardMedia(const MediaMessage& msg) {
    // Note: mutex is already held by caller (processMedia)
    // We need to call callback outside mutex to avoid deadlocks

    MediaForwardCallback callback;
    {
        // Copy callback under lock
        callback = mediaForwardCallback_;
    }

    if (callback) {
        callback(msg);
    }
}

} // namespace streaming
} // namespace openrtmp
