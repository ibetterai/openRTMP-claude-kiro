// OpenRTMP - Cross-platform RTMP Server
// Media Handler - Processes audio (type 8), video (type 9), and data (type 18) messages
//
// Responsibilities:
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

#ifndef OPENRTMP_STREAMING_MEDIA_HANDLER_HPP
#define OPENRTMP_STREAMING_MEDIA_HANDLER_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <memory>
#include <mutex>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/result.hpp"

namespace openrtmp {
namespace streaming {

// =============================================================================
// Media Constants
// =============================================================================

namespace media {
    // RTMP Message Type IDs
    constexpr uint8_t MSG_TYPE_AUDIO = 8;
    constexpr uint8_t MSG_TYPE_VIDEO = 9;
    constexpr uint8_t MSG_TYPE_DATA = 18;

    // FLV Video Frame Types (upper 4 bits of first byte)
    constexpr uint8_t FRAME_TYPE_KEYFRAME = 1;
    constexpr uint8_t FRAME_TYPE_INTER_FRAME = 2;
    constexpr uint8_t FRAME_TYPE_DISPOSABLE_INTER = 3;
    constexpr uint8_t FRAME_TYPE_GENERATED_KEYFRAME = 4;
    constexpr uint8_t FRAME_TYPE_VIDEO_INFO = 5;

    // FLV Video Codec IDs (lower 4 bits of first byte, when not Enhanced RTMP)
    constexpr uint8_t CODEC_ID_H263 = 2;
    constexpr uint8_t CODEC_ID_SCREEN_VIDEO = 3;
    constexpr uint8_t CODEC_ID_VP6 = 4;
    constexpr uint8_t CODEC_ID_VP6_ALPHA = 5;
    constexpr uint8_t CODEC_ID_SCREEN_VIDEO2 = 6;
    constexpr uint8_t CODEC_ID_AVC = 7;

    // AVC Packet Types
    constexpr uint8_t AVC_PACKET_TYPE_SEQUENCE_HEADER = 0;
    constexpr uint8_t AVC_PACKET_TYPE_NALU = 1;
    constexpr uint8_t AVC_PACKET_TYPE_END_OF_SEQUENCE = 2;

    // H.264/AVC NAL Unit Types
    constexpr uint8_t AVC_NAL_TYPE_NON_IDR = 1;
    constexpr uint8_t AVC_NAL_TYPE_IDR = 5;
    constexpr uint8_t AVC_NAL_TYPE_SEI = 6;
    constexpr uint8_t AVC_NAL_TYPE_SPS = 7;
    constexpr uint8_t AVC_NAL_TYPE_PPS = 8;

    // H.265/HEVC NAL Unit Types
    constexpr uint8_t HEVC_NAL_TYPE_TRAIL_N = 0;
    constexpr uint8_t HEVC_NAL_TYPE_TRAIL_R = 1;
    constexpr uint8_t HEVC_NAL_TYPE_BLA_W_LP = 16;
    constexpr uint8_t HEVC_NAL_TYPE_IDR_W_RADL = 19;
    constexpr uint8_t HEVC_NAL_TYPE_IDR_N_LP = 20;
    constexpr uint8_t HEVC_NAL_TYPE_CRA_NUT = 21;
    constexpr uint8_t HEVC_NAL_TYPE_VPS = 32;
    constexpr uint8_t HEVC_NAL_TYPE_SPS = 33;
    constexpr uint8_t HEVC_NAL_TYPE_PPS = 34;

    // FLV Audio Format (upper 4 bits of first byte)
    constexpr uint8_t AUDIO_FORMAT_AAC = 10;
    constexpr uint8_t AUDIO_FORMAT_OPUS = 16;  // Enhanced RTMP

    // AAC Packet Types
    constexpr uint8_t AAC_PACKET_TYPE_SEQUENCE_HEADER = 0;
    constexpr uint8_t AAC_PACKET_TYPE_RAW = 1;

    // Enhanced RTMP flags
    constexpr uint8_t ENHANCED_RTMP_FLAG = 0x80;  // IsExHeader flag in upper nibble

    // Enhanced RTMP Packet Types
    constexpr uint8_t EX_PACKET_TYPE_SEQUENCE_START = 0;
    constexpr uint8_t EX_PACKET_TYPE_CODED_FRAMES = 1;
    constexpr uint8_t EX_PACKET_TYPE_SEQUENCE_END = 2;
    constexpr uint8_t EX_PACKET_TYPE_CODED_FRAMES_X = 3;
    constexpr uint8_t EX_PACKET_TYPE_METADATA = 4;
    constexpr uint8_t EX_PACKET_TYPE_MPEG2TS_SEQUENCE_START = 5;

    // Common FourCC codes
    constexpr char FOURCC_AVC1[] = "avc1";
    constexpr char FOURCC_HVC1[] = "hvc1";
    constexpr char FOURCC_AV01[] = "av01";
    constexpr char FOURCC_VP09[] = "vp09";
}

// =============================================================================
// Media Message Structure
// =============================================================================

/**
 * @brief Media message containing audio, video, or data payload.
 */
struct MediaMessage {
    MediaType type;                 ///< Media type (Audio, Video, Data)
    uint32_t timestamp;             ///< Presentation timestamp (milliseconds)
    std::vector<uint8_t> payload;   ///< Raw payload data
    bool isKeyframe;                ///< Whether this is a keyframe (video only)

    MediaMessage()
        : type(MediaType::Video)
        , timestamp(0)
        , isKeyframe(false)
    {}
};

// =============================================================================
// Error Types
// =============================================================================

/**
 * @brief Media handler error information.
 */
struct MediaHandlerError {
    /**
     * @brief Error codes for media handler operations.
     */
    enum class Code {
        EmptyPayload,           ///< Payload is empty
        InvalidHeader,          ///< Invalid header format
        UnsupportedCodec,       ///< Unsupported codec type
        MalformedData,          ///< Data is malformed
        InternalError           ///< Internal error occurred
    };

    Code code;              ///< Error code
    std::string message;    ///< Human-readable error message

    MediaHandlerError(Code c = Code::InternalError, std::string msg = "")
        : code(c), message(std::move(msg)) {}
};

// =============================================================================
// Callback Types
// =============================================================================

/**
 * @brief Callback invoked when a keyframe is detected.
 * @param timestamp The timestamp of the keyframe
 */
using KeyframeCallback = std::function<void(uint32_t timestamp)>;

/**
 * @brief Callback invoked when metadata is received.
 * @param data The raw metadata payload
 */
using MetadataCallback = std::function<void(const std::vector<uint8_t>& data)>;

/**
 * @brief Callback invoked to forward validated media.
 * @param msg The validated media message
 */
using MediaForwardCallback = std::function<void(const MediaMessage& msg)>;

// =============================================================================
// Media Handler Interface
// =============================================================================

/**
 * @brief Interface for media handling operations.
 *
 * Defines the contract for processing media messages.
 */
class IMediaHandler {
public:
    virtual ~IMediaHandler() = default;

    /**
     * @brief Process a media message.
     *
     * @param msg The media message to process
     * @return Result indicating success or error
     */
    virtual core::Result<void, MediaHandlerError> processMedia(
        const MediaMessage& msg
    ) = 0;

    // -------------------------------------------------------------------------
    // State Queries
    // -------------------------------------------------------------------------

    /**
     * @brief Check if video sequence header has been received.
     * @return true if sequence header available
     */
    virtual bool hasVideoSequenceHeader() const = 0;

    /**
     * @brief Check if audio sequence header has been received.
     * @return true if sequence header available
     */
    virtual bool hasAudioSequenceHeader() const = 0;

    /**
     * @brief Check if metadata has been received.
     * @return true if metadata available
     */
    virtual bool hasMetadata() const = 0;

    /**
     * @brief Check if Enhanced RTMP is in use.
     * @return true if Enhanced RTMP detected
     */
    virtual bool isEnhancedRTMP() const = 0;

    /**
     * @brief Check if last video frame was a keyframe.
     * @return true if last frame was keyframe
     */
    virtual bool isLastFrameKeyframe() const = 0;

    // -------------------------------------------------------------------------
    // Codec Information
    // -------------------------------------------------------------------------

    /**
     * @brief Get the detected video codec.
     * @return Video codec type
     */
    virtual VideoCodec getVideoCodec() const = 0;

    /**
     * @brief Get the detected audio codec.
     * @return Audio codec type
     */
    virtual AudioCodec getAudioCodec() const = 0;

    /**
     * @brief Get the FourCC code (Enhanced RTMP).
     * @return FourCC string or empty if not Enhanced RTMP
     */
    virtual std::string getFourCC() const = 0;

    /**
     * @brief Get the last NAL unit type (for AVC/HEVC).
     * @return NAL unit type
     */
    virtual uint8_t getLastNALUnitType() const = 0;

    /**
     * @brief Get audio sample rate.
     * @return Sample rate in Hz
     */
    virtual uint32_t getAudioSampleRate() const = 0;

    /**
     * @brief Get audio channel count.
     * @return Number of audio channels
     */
    virtual uint8_t getAudioChannels() const = 0;

    /**
     * @brief Get complete codec information.
     * @return CodecInfo structure
     */
    virtual CodecInfo getCodecInfo() const = 0;

    // -------------------------------------------------------------------------
    // Sequence Header Access
    // -------------------------------------------------------------------------

    /**
     * @brief Get cached video sequence header.
     * @return Video sequence header data
     */
    virtual std::vector<uint8_t> getVideoSequenceHeader() const = 0;

    /**
     * @brief Get cached audio sequence header.
     * @return Audio sequence header data
     */
    virtual std::vector<uint8_t> getAudioSequenceHeader() const = 0;

    // -------------------------------------------------------------------------
    // Callbacks
    // -------------------------------------------------------------------------

    /**
     * @brief Set callback for keyframe detection.
     * @param callback The callback to invoke
     */
    virtual void setKeyframeCallback(KeyframeCallback callback) = 0;

    /**
     * @brief Set callback for metadata reception.
     * @param callback The callback to invoke
     */
    virtual void setMetadataCallback(MetadataCallback callback) = 0;

    /**
     * @brief Set callback for media forwarding.
     * @param callback The callback to invoke
     */
    virtual void setMediaForwardCallback(MediaForwardCallback callback) = 0;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Reset all state.
     */
    virtual void reset() = 0;
};

// =============================================================================
// Media Handler Implementation
// =============================================================================

/**
 * @brief Media handler implementation.
 *
 * Implements IMediaHandler interface for processing RTMP media messages.
 *
 * Features:
 * - Parse H.264/AVC and H.265/HEVC video
 * - Parse AAC audio
 * - Support Enhanced RTMP with FourCC detection
 * - Keyframe detection from NAL unit types
 * - Sequence header caching
 * - Metadata extraction
 * - Thread-safe operation
 */
class MediaHandler : public IMediaHandler {
public:
    /**
     * @brief Construct a new MediaHandler.
     */
    MediaHandler();

    /**
     * @brief Destructor.
     */
    ~MediaHandler() override = default;

    // Non-copyable
    MediaHandler(const MediaHandler&) = delete;
    MediaHandler& operator=(const MediaHandler&) = delete;

    // Movable
    MediaHandler(MediaHandler&&) noexcept;
    MediaHandler& operator=(MediaHandler&&) noexcept;

    // IMediaHandler interface
    core::Result<void, MediaHandlerError> processMedia(
        const MediaMessage& msg
    ) override;

    bool hasVideoSequenceHeader() const override;
    bool hasAudioSequenceHeader() const override;
    bool hasMetadata() const override;
    bool isEnhancedRTMP() const override;
    bool isLastFrameKeyframe() const override;

    VideoCodec getVideoCodec() const override;
    AudioCodec getAudioCodec() const override;
    std::string getFourCC() const override;
    uint8_t getLastNALUnitType() const override;
    uint32_t getAudioSampleRate() const override;
    uint8_t getAudioChannels() const override;
    CodecInfo getCodecInfo() const override;

    std::vector<uint8_t> getVideoSequenceHeader() const override;
    std::vector<uint8_t> getAudioSequenceHeader() const override;

    void setKeyframeCallback(KeyframeCallback callback) override;
    void setMetadataCallback(MetadataCallback callback) override;
    void setMediaForwardCallback(MediaForwardCallback callback) override;

    void reset() override;

private:
    // -------------------------------------------------------------------------
    // Processing Helpers
    // -------------------------------------------------------------------------

    /**
     * @brief Process video message.
     */
    core::Result<void, MediaHandlerError> processVideo(const MediaMessage& msg);

    /**
     * @brief Process audio message.
     */
    core::Result<void, MediaHandlerError> processAudio(const MediaMessage& msg);

    /**
     * @brief Process data message.
     */
    core::Result<void, MediaHandlerError> processData(const MediaMessage& msg);

    /**
     * @brief Process standard (non-Enhanced) RTMP video.
     */
    core::Result<void, MediaHandlerError> processStandardVideo(
        const MediaMessage& msg,
        uint8_t firstByte
    );

    /**
     * @brief Process Enhanced RTMP video.
     */
    core::Result<void, MediaHandlerError> processEnhancedVideo(
        const MediaMessage& msg
    );

    /**
     * @brief Parse AVC/H.264 sequence header.
     */
    void parseAVCSequenceHeader(const std::vector<uint8_t>& payload);

    /**
     * @brief Parse HEVC/H.265 sequence header.
     */
    void parseHEVCSequenceHeader(const std::vector<uint8_t>& payload);

    /**
     * @brief Parse AAC sequence header.
     */
    void parseAACSequenceHeader(const std::vector<uint8_t>& payload);

    /**
     * @brief Extract NAL unit type from AVC data.
     */
    uint8_t extractAVCNALType(const std::vector<uint8_t>& payload, size_t offset);

    /**
     * @brief Extract NAL unit type from HEVC data.
     */
    uint8_t extractHEVCNALType(const std::vector<uint8_t>& payload, size_t offset);

    /**
     * @brief Check if NAL type is an IDR (keyframe) for AVC.
     */
    bool isAVCKeyframeNAL(uint8_t nalType) const;

    /**
     * @brief Check if NAL type is a keyframe for HEVC.
     */
    bool isHEVCKeyframeNAL(uint8_t nalType) const;

    /**
     * @brief Forward media to callback.
     */
    void forwardMedia(const MediaMessage& msg);

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    // Video state
    VideoCodec videoCodec_;
    bool hasVideoSeqHeader_;
    bool isEnhancedRTMP_;
    bool lastFrameIsKeyframe_;
    uint8_t lastNALUnitType_;
    std::string fourCC_;
    std::vector<uint8_t> videoSequenceHeader_;

    // Audio state
    AudioCodec audioCodec_;
    bool hasAudioSeqHeader_;
    uint32_t audioSampleRate_;
    uint8_t audioChannels_;
    std::vector<uint8_t> audioSequenceHeader_;

    // Metadata state
    bool hasMetadata_;
    std::vector<uint8_t> metadata_;

    // Callbacks
    KeyframeCallback keyframeCallback_;
    MetadataCallback metadataCallback_;
    MediaForwardCallback mediaForwardCallback_;

    // Thread safety
    mutable std::mutex mutex_;
};

} // namespace streaming
} // namespace openrtmp

#endif // OPENRTMP_STREAMING_MEDIA_HANDLER_HPP
