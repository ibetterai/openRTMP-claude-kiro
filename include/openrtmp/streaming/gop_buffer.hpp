// OpenRTMP - Cross-platform RTMP Server
// GOP Buffer - Maintains a circular buffer of media frames for instant playback
//
// Responsibilities:
// - Maintain minimum 2 seconds of buffered media per stream
// - Index keyframe positions for instant playback start
// - Store metadata and codec sequence headers separately
// - Implement reference-counted buffer sharing for distribution
// - Support configurable buffer duration for low-latency mode
//
// Requirements coverage:
// - Requirement 4.6: GOP buffer of at least 2 seconds for instant playback
// - Requirement 5.1: Transmit data starting from most recent keyframe
// - Requirement 5.3: Send cached metadata and sequence headers before stream data

#ifndef OPENRTMP_STREAMING_GOP_BUFFER_HPP
#define OPENRTMP_STREAMING_GOP_BUFFER_HPP

#include <cstdint>
#include <vector>
#include <optional>
#include <chrono>
#include <memory>
#include <mutex>
#include <deque>

#include "openrtmp/core/types.hpp"
#include "openrtmp/protocol/amf_codec.hpp"

namespace openrtmp {
namespace streaming {

// =============================================================================
// Data Structures
// =============================================================================

/**
 * @brief A buffered media frame with metadata.
 *
 * Contains the frame data along with timing and type information
 * needed for GOP buffer management and distribution.
 */
struct BufferedFrame {
    MediaType type;                 ///< Media type (Audio, Video, Data)
    uint32_t timestamp;             ///< Presentation timestamp (milliseconds)
    std::vector<uint8_t> data;      ///< Frame payload data
    bool isKeyframe;                ///< True if this is a keyframe (IDR for video)

    BufferedFrame()
        : type(MediaType::Video)
        , timestamp(0)
        , isKeyframe(false)
    {}
};

/**
 * @brief Sequence headers for video and audio streams.
 *
 * These headers are required to initialize decoders and must be
 * sent to new subscribers before stream data.
 */
struct SequenceHeaders {
    std::vector<uint8_t> videoHeader;   ///< Video sequence header (e.g., AVCDecoderConfigurationRecord)
    std::vector<uint8_t> audioHeader;   ///< Audio sequence header (e.g., AudioSpecificConfig)
};

/**
 * @brief Configuration for GOP buffer behavior.
 */
struct GOPBufferConfig {
    std::chrono::milliseconds minBufferDuration{2000};  ///< Minimum buffer duration (default 2 seconds)
    size_t maxBufferSize{10 * 1024 * 1024};             ///< Maximum buffer size in bytes (default 10MB)

    GOPBufferConfig() = default;
};

// =============================================================================
// GOP Buffer Interface
// =============================================================================

/**
 * @brief Interface for GOP buffer operations.
 *
 * The GOP buffer maintains a sliding window of media frames, always
 * starting from the most recent keyframe, enabling instant playback
 * for new subscribers.
 */
class IGOPBuffer {
public:
    virtual ~IGOPBuffer() = default;

    // -------------------------------------------------------------------------
    // Frame Management
    // -------------------------------------------------------------------------

    /**
     * @brief Push a frame into the buffer.
     *
     * Frames are added to the buffer and old frames are evicted
     * according to the buffer configuration. Keyframes are indexed
     * for efficient retrieval.
     *
     * @param frame The frame to add
     */
    virtual void push(const BufferedFrame& frame) = 0;

    // -------------------------------------------------------------------------
    // Metadata and Headers
    // -------------------------------------------------------------------------

    /**
     * @brief Set stream metadata (e.g., onMetaData).
     *
     * Metadata is stored separately and persists through frame eviction.
     *
     * @param metadata The AMF-encoded metadata
     */
    virtual void setMetadata(const protocol::AMFValue& metadata) = 0;

    /**
     * @brief Set codec sequence headers.
     *
     * Headers are stored separately and persists through frame eviction.
     *
     * @param videoHeader Video sequence header
     * @param audioHeader Audio sequence header
     */
    virtual void setSequenceHeaders(
        const std::vector<uint8_t>& videoHeader,
        const std::vector<uint8_t>& audioHeader
    ) = 0;

    // -------------------------------------------------------------------------
    // Retrieval
    // -------------------------------------------------------------------------

    /**
     * @brief Get metadata if available.
     *
     * @return Optional containing metadata, or empty if not set
     */
    virtual std::optional<protocol::AMFValue> getMetadata() const = 0;

    /**
     * @brief Get sequence headers.
     *
     * @return Sequence headers (may have empty vectors if not set)
     */
    virtual SequenceHeaders getSequenceHeaders() const = 0;

    /**
     * @brief Get frames from the most recent keyframe.
     *
     * Returns all frames from the last keyframe to the current
     * position, enabling instant playback for new subscribers.
     * (Requirement 5.1)
     *
     * @return Vector of frames from last keyframe, empty if no keyframe exists
     */
    virtual std::vector<BufferedFrame> getFromLastKeyframe() const = 0;

    // -------------------------------------------------------------------------
    // Statistics
    // -------------------------------------------------------------------------

    /**
     * @brief Get the duration of buffered content.
     *
     * @return Duration from oldest to newest frame timestamp
     */
    virtual std::chrono::milliseconds getBufferedDuration() const = 0;

    /**
     * @brief Get total bytes currently buffered.
     *
     * @return Size in bytes
     */
    virtual size_t getBufferedBytes() const = 0;

    /**
     * @brief Get number of frames in buffer.
     *
     * @return Frame count
     */
    virtual size_t getFrameCount() const = 0;

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    /**
     * @brief Set buffer configuration.
     *
     * @param config New configuration
     */
    virtual void setConfig(const GOPBufferConfig& config) = 0;

    /**
     * @brief Get current configuration.
     *
     * @return Current configuration
     */
    virtual GOPBufferConfig getConfig() const = 0;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Clear all frames but preserve metadata and headers.
     */
    virtual void clear() = 0;

    /**
     * @brief Reset to initial state, clearing everything.
     */
    virtual void reset() = 0;
};

// =============================================================================
// GOP Buffer Implementation
// =============================================================================

/**
 * @brief Thread-safe circular GOP buffer implementation.
 *
 * Implements IGOPBuffer with:
 * - Circular buffer with automatic eviction of old frames
 * - Keyframe index tracking for instant playback
 * - Separate storage for metadata and sequence headers
 * - Configurable buffer duration for low-latency mode
 * - Thread-safe access using mutex protection
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses std::mutex for read/write synchronization
 * - Supports single producer, multiple consumer pattern
 */
class GOPBuffer : public IGOPBuffer {
public:
    /**
     * @brief Construct a new GOPBuffer with default configuration.
     */
    GOPBuffer();

    /**
     * @brief Construct a new GOPBuffer with custom configuration.
     *
     * @param config Buffer configuration
     */
    explicit GOPBuffer(const GOPBufferConfig& config);

    /**
     * @brief Destructor.
     */
    ~GOPBuffer() override = default;

    // Non-copyable
    GOPBuffer(const GOPBuffer&) = delete;
    GOPBuffer& operator=(const GOPBuffer&) = delete;

    // Movable
    GOPBuffer(GOPBuffer&&) noexcept;
    GOPBuffer& operator=(GOPBuffer&&) noexcept;

    // IGOPBuffer interface implementation
    void push(const BufferedFrame& frame) override;
    void setMetadata(const protocol::AMFValue& metadata) override;
    void setSequenceHeaders(
        const std::vector<uint8_t>& videoHeader,
        const std::vector<uint8_t>& audioHeader
    ) override;

    std::optional<protocol::AMFValue> getMetadata() const override;
    SequenceHeaders getSequenceHeaders() const override;
    std::vector<BufferedFrame> getFromLastKeyframe() const override;

    std::chrono::milliseconds getBufferedDuration() const override;
    size_t getBufferedBytes() const override;
    size_t getFrameCount() const override;

    void setConfig(const GOPBufferConfig& config) override;
    GOPBufferConfig getConfig() const override;

    void clear() override;
    void reset() override;

private:
    /**
     * @brief Evict old frames to maintain buffer constraints.
     *
     * Called internally after pushing new frames.
     */
    void evictOldFrames();

    /**
     * @brief Update keyframe index after push or eviction.
     */
    void updateKeyframeIndex();

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    // Frame storage
    std::deque<BufferedFrame> frames_;

    // Keyframe tracking
    size_t lastKeyframeIndex_;      ///< Index of last keyframe in frames_
    bool hasKeyframe_;              ///< Whether any keyframe exists in buffer

    // Metadata and headers (stored separately)
    std::optional<protocol::AMFValue> metadata_;
    SequenceHeaders sequenceHeaders_;

    // Statistics
    size_t bufferedBytes_;

    // Configuration
    GOPBufferConfig config_;

    // Thread safety
    mutable std::mutex mutex_;
};

} // namespace streaming
} // namespace openrtmp

#endif // OPENRTMP_STREAMING_GOP_BUFFER_HPP
