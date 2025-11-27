// OpenRTMP - Cross-platform RTMP Server
// RTMP Chunk Stream Parser - Parses RTMP chunk stream format from raw bytes
//
// Implements RTMP chunk parsing per Adobe specification:
// - Basic Header (1-3 bytes) for chunk stream ID and format type
// - Message Header (0-11 bytes) based on format type
// - Dynamic chunk size handling (128 to 65536 bytes)
// - Per-chunk-stream state for header compression
//
// Chunk format:
// +-------------+----------------+-------------------+
// | Basic Header| Message Header |    Chunk Data     |
// | (1-3 bytes) | (0/3/7/11 bytes)|   (variable)     |
// +-------------+----------------+-------------------+

#ifndef OPENRTMP_PROTOCOL_CHUNK_PARSER_HPP
#define OPENRTMP_PROTOCOL_CHUNK_PARSER_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <optional>
#include <string>
#include <memory>
#include <unordered_map>

namespace openrtmp {
namespace protocol {

// RTMP Protocol Control Message Type IDs
namespace chunk {
    constexpr uint8_t MSG_TYPE_SET_CHUNK_SIZE = 1;
    constexpr uint8_t MSG_TYPE_ABORT = 2;
    constexpr uint8_t MSG_TYPE_ACKNOWLEDGEMENT = 3;
    constexpr uint8_t MSG_TYPE_USER_CONTROL = 4;
    constexpr uint8_t MSG_TYPE_WINDOW_ACK_SIZE = 5;
    constexpr uint8_t MSG_TYPE_SET_PEER_BANDWIDTH = 6;

    // Default and limits per RTMP specification
    constexpr uint32_t DEFAULT_CHUNK_SIZE = 128;
    constexpr uint32_t MIN_CHUNK_SIZE = 128;
    constexpr uint32_t MAX_CHUNK_SIZE = 65536;

    // Extended timestamp marker
    constexpr uint32_t EXTENDED_TIMESTAMP_MARKER = 0xFFFFFF;
}

/**
 * @brief Parse error information.
 */
struct ParseError {
    /**
     * @brief Error codes for chunk parsing failures.
     */
    enum class Code {
        InvalidBasicHeader,     ///< Basic header structure invalid
        InvalidMessageHeader,   ///< Message header structure invalid
        InvalidChunkSize,       ///< Chunk size out of valid range
        InvalidTimestamp,       ///< Timestamp value invalid
        MalformedProtocolMsg,   ///< Protocol control message malformed
        BufferOverflow,         ///< Internal buffer overflow
        UnknownChunkStream      ///< Reference to unknown chunk stream (Type 3)
    };

    Code code;             ///< Error code
    std::string message;   ///< Human-readable error message

    ParseError(Code c = Code::InvalidBasicHeader, std::string msg = "")
        : code(c), message(std::move(msg)) {}
};

/**
 * @brief Result of parsing chunk data.
 */
struct ParseResult {
    size_t bytesConsumed;                 ///< Number of bytes consumed from input
    size_t chunksCompleted;               ///< Number of complete chunks parsed
    std::optional<ParseError> error;      ///< Error information if failed

    /**
     * @brief Create a successful result.
     * @param consumed Number of bytes consumed
     * @param completed Number of chunks completed
     * @return Successful ParseResult
     */
    static ParseResult ok(size_t consumed, size_t completed = 0) {
        return ParseResult{consumed, completed, std::nullopt};
    }

    /**
     * @brief Create a failed result.
     * @param err Error information
     * @return Failed ParseResult
     */
    static ParseResult fail(ParseError err) {
        return ParseResult{0, 0, std::move(err)};
    }
};

/**
 * @brief Parsed chunk data structure.
 *
 * Contains all header information and payload data for a parsed chunk.
 */
struct ChunkData {
    uint32_t chunkStreamId;               ///< Chunk stream ID (2-65599)
    uint32_t timestamp;                   ///< Absolute timestamp (milliseconds)
    uint32_t messageLength;               ///< Total message length (bytes)
    uint8_t messageTypeId;                ///< Message type ID
    uint32_t messageStreamId;             ///< Message stream ID
    std::vector<uint8_t> payload;         ///< Message payload data
    bool isComplete;                      ///< Full message payload received

    ChunkData()
        : chunkStreamId(0)
        , timestamp(0)
        , messageLength(0)
        , messageTypeId(0)
        , messageStreamId(0)
        , isComplete(false) {}
};

/**
 * @brief Interface for RTMP chunk stream parser.
 *
 * This interface defines the contract for parsing RTMP chunks from raw bytes.
 */
class IChunkParser {
public:
    virtual ~IChunkParser() = default;

    /**
     * @brief Feed raw bytes into parser.
     *
     * Parses RTMP chunks from the input data. Handles:
     * - Basic Header (1-3 bytes) to extract chunk stream ID and format type
     * - Message Header (0/3/7/11 bytes) based on format type
     * - Chunk payload up to current chunk size
     *
     * @param data Pointer to input data
     * @param length Length of input data
     * @return ParseResult indicating bytes consumed and any errors
     */
    virtual ParseResult parse(const uint8_t* data, size_t length) = 0;

    /**
     * @brief Update chunk size for this stream direction.
     *
     * Sets the maximum chunk payload size. Valid range is 128-65536 bytes.
     * Values outside this range will be clamped.
     *
     * @param size New chunk size in bytes
     */
    virtual void setChunkSize(uint32_t size) = 0;

    /**
     * @brief Abort partial message on chunk stream.
     *
     * Discards any partially received message data for the specified
     * chunk stream ID. Used when receiving an Abort Message (type 2).
     *
     * @param chunkStreamId Chunk stream ID to abort
     */
    virtual void abortChunkStream(uint32_t chunkStreamId) = 0;

    /**
     * @brief Get parsed chunks ready for assembly.
     *
     * Returns all completed chunks since last call. Clears internal list
     * after returning.
     *
     * @return Vector of completed chunk data
     */
    virtual std::vector<ChunkData> getCompletedChunks() = 0;
};

/**
 * @brief Per-chunk-stream state for header compression.
 *
 * Tracks previous header values for each chunk stream ID to support
 * Type 1, 2, and 3 header compression.
 */
struct ChunkStreamState {
    uint32_t timestamp;           ///< Last absolute timestamp
    uint32_t timestampDelta;      ///< Last timestamp delta
    uint32_t messageLength;       ///< Last message length
    uint8_t messageTypeId;        ///< Last message type ID
    uint32_t messageStreamId;     ///< Last message stream ID
    bool hasExtendedTimestamp;    ///< Whether last chunk had extended timestamp

    // Partial message buffer for multi-chunk messages
    std::vector<uint8_t> partialPayload;
    uint32_t remainingBytes;      ///< Bytes remaining to complete message

    ChunkStreamState()
        : timestamp(0)
        , timestampDelta(0)
        , messageLength(0)
        , messageTypeId(0)
        , messageStreamId(0)
        , hasExtendedTimestamp(false)
        , remainingBytes(0) {}

    /**
     * @brief Reset partial message state.
     */
    void resetPartial() {
        partialPayload.clear();
        remainingBytes = 0;
    }
};

/**
 * @brief RTMP Chunk Stream Parser implementation.
 *
 * Implements the IChunkParser interface for parsing RTMP chunks from raw bytes.
 *
 * Protocol overview:
 * - Basic Header (1-3 bytes):
 *   - fmt (2 bits): Chunk type (0-3) determining message header size
 *   - cs id (6-22 bits): Chunk stream ID
 *
 * - Message Header (varies by fmt):
 *   - Type 0 (11 bytes): Full header
 *   - Type 1 (7 bytes): Omits message stream id
 *   - Type 2 (3 bytes): Only timestamp delta
 *   - Type 3 (0 bytes): Uses all stored values
 *
 * Requirements coverage:
 * - Requirement 2.1: Support chunk sizes from 128 to 65536 bytes
 * - Requirement 2.2: Handle Set Chunk Size message
 * - Requirement 2.5: Handle Abort Message to discard partial messages
 */
class ChunkParser : public IChunkParser {
public:
    /**
     * @brief Construct a new ChunkParser.
     *
     * Initializes parser with default chunk size of 128 bytes.
     */
    ChunkParser();

    /**
     * @brief Destructor.
     */
    ~ChunkParser() override = default;

    // Non-copyable but movable
    ChunkParser(const ChunkParser&) = delete;
    ChunkParser& operator=(const ChunkParser&) = delete;
    ChunkParser(ChunkParser&&) noexcept = default;
    ChunkParser& operator=(ChunkParser&&) noexcept = default;

    // IChunkParser interface
    ParseResult parse(const uint8_t* data, size_t length) override;
    void setChunkSize(uint32_t size) override;
    void abortChunkStream(uint32_t chunkStreamId) override;
    std::vector<ChunkData> getCompletedChunks() override;

private:
    /**
     * @brief Parse basic header from data.
     *
     * @param data Pointer to data
     * @param length Available data length
     * @param[out] fmt Format type (0-3)
     * @param[out] csid Chunk stream ID
     * @param[out] headerSize Size of basic header consumed
     * @return true if header fully parsed, false if more data needed
     */
    bool parseBasicHeader(
        const uint8_t* data,
        size_t length,
        uint8_t& fmt,
        uint32_t& csid,
        size_t& headerSize
    );

    /**
     * @brief Parse message header based on format type.
     *
     * @param fmt Format type (0-3)
     * @param data Pointer to data (after basic header)
     * @param length Available data length
     * @param state Chunk stream state to update
     * @param[out] headerSize Size of message header consumed
     * @return true if header fully parsed, false if more data needed
     */
    bool parseMessageHeader(
        uint8_t fmt,
        const uint8_t* data,
        size_t length,
        ChunkStreamState& state,
        size_t& headerSize
    );

    /**
     * @brief Process protocol control messages.
     *
     * Handles Set Chunk Size (type 1) and Abort (type 2) messages.
     *
     * @param chunk The completed chunk to process
     */
    void processProtocolControlMessage(const ChunkData& chunk);

    /**
     * @brief Get or create chunk stream state.
     *
     * @param csid Chunk stream ID
     * @return Reference to state for this chunk stream
     */
    ChunkStreamState& getChunkStreamState(uint32_t csid);

    /**
     * @brief Check if chunk stream has been initialized.
     *
     * @param csid Chunk stream ID
     * @return true if state exists for this chunk stream
     */
    bool hasChunkStreamState(uint32_t csid) const;

    uint32_t chunkSize_;                                          ///< Current chunk size
    std::unordered_map<uint32_t, ChunkStreamState> chunkStreams_; ///< Per-stream state
    std::vector<ChunkData> completedChunks_;                      ///< Completed chunks
};

} // namespace protocol
} // namespace openrtmp

#endif // OPENRTMP_PROTOCOL_CHUNK_PARSER_HPP
