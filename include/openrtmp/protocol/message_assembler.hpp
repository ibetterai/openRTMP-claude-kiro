// OpenRTMP - Cross-platform RTMP Server
// RTMP Message Assembler - Reassembles complete messages from chunks
//
// Implements message reassembly per RTMP specification:
// - Buffers partial message data across multiple chunks
// - Reassembles complete messages before forwarding for processing
// - Supports all standard RTMP message types (audio 8, video 9, data 18, command 20)
// - Logs unknown message types and continues processing
// - Validates message integrity and length consistency
//
// Requirements coverage:
// - Requirement 2.3: Reassemble complete messages from multiple chunks
// - Requirement 2.4: Support all standard RTMP message types
// - Requirement 2.6: Log unknown message types and continue processing

#ifndef OPENRTMP_PROTOCOL_MESSAGE_ASSEMBLER_HPP
#define OPENRTMP_PROTOCOL_MESSAGE_ASSEMBLER_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <optional>
#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include "openrtmp/protocol/chunk_parser.hpp"
#include "openrtmp/core/result.hpp"

namespace openrtmp {
namespace protocol {

/**
 * @brief RTMP message type enumeration.
 *
 * Categorizes RTMP messages by their functional type.
 */
enum class MessageType {
    ProtocolControl,   ///< Protocol control messages (types 1-6)
    Audio,             ///< Audio data (type 8)
    Video,             ///< Video data (type 9)
    Data,              ///< Data/metadata (types 15, 18)
    Command,           ///< Command messages (types 17, 20)
    SharedObject,      ///< Shared object (types 16, 19)
    Aggregate,         ///< Aggregate message (type 22)
    Unknown            ///< Unknown message type
};

/**
 * @brief RTMP Message Type IDs.
 */
namespace message {
    // Protocol control messages (message stream ID 0)
    constexpr uint8_t TYPE_SET_CHUNK_SIZE = 1;
    constexpr uint8_t TYPE_ABORT = 2;
    constexpr uint8_t TYPE_ACKNOWLEDGEMENT = 3;
    constexpr uint8_t TYPE_USER_CONTROL = 4;
    constexpr uint8_t TYPE_WINDOW_ACK_SIZE = 5;
    constexpr uint8_t TYPE_SET_PEER_BANDWIDTH = 6;

    // Media and command messages
    constexpr uint8_t TYPE_AUDIO = 8;
    constexpr uint8_t TYPE_VIDEO = 9;

    // Data messages
    constexpr uint8_t TYPE_DATA_AMF3 = 15;
    constexpr uint8_t TYPE_DATA_AMF0 = 18;

    // Shared object messages
    constexpr uint8_t TYPE_SHARED_OBJECT_AMF3 = 16;
    constexpr uint8_t TYPE_SHARED_OBJECT_AMF0 = 19;

    // Command messages
    constexpr uint8_t TYPE_COMMAND_AMF3 = 17;
    constexpr uint8_t TYPE_COMMAND_AMF0 = 20;

    // Aggregate message
    constexpr uint8_t TYPE_AGGREGATE = 22;
}

/**
 * @brief Reassembled RTMP message structure.
 *
 * Contains all message information after chunk reassembly.
 */
struct RTMPMessage {
    MessageType messageType;              ///< Categorized message type
    uint8_t messageTypeId;                ///< Original message type ID
    uint32_t timestamp;                   ///< Message timestamp (milliseconds)
    uint32_t messageStreamId;             ///< Message stream ID
    uint32_t chunkStreamId;               ///< Original chunk stream ID
    std::vector<uint8_t> payload;         ///< Complete message payload

    RTMPMessage()
        : messageType(MessageType::Unknown)
        , messageTypeId(0)
        , timestamp(0)
        , messageStreamId(0)
        , chunkStreamId(0) {}
};

/**
 * @brief Error information for message assembly failures.
 */
struct AssemblyError {
    /**
     * @brief Error codes for message assembly failures.
     */
    enum class Code {
        LengthMismatch,         ///< Payload length doesn't match declared length
        InvalidChunkStream,     ///< Invalid chunk stream ID
        BufferOverflow,         ///< Message buffer overflow
        InternalError           ///< Internal processing error
    };

    Code code;                  ///< Error code
    std::string message;        ///< Human-readable error message

    AssemblyError(Code c = Code::InternalError, std::string msg = "")
        : code(c), message(std::move(msg)) {}
};

/**
 * @brief Result type for message assembly operations.
 */
using AssemblyResult = core::Result<void, AssemblyError>;

/**
 * @brief Callback type for completed messages.
 */
using MessageCallback = std::function<void(const RTMPMessage&)>;

/**
 * @brief Callback type for unknown message type logging.
 */
using UnknownTypeCallback = std::function<void(uint8_t)>;

/**
 * @brief Interface for RTMP message assembler.
 *
 * This interface defines the contract for reassembling RTMP messages from chunks.
 */
class IMessageAssembler {
public:
    virtual ~IMessageAssembler() = default;

    /**
     * @brief Process a chunk from the ChunkParser.
     *
     * Buffers chunk data and reassembles complete messages.
     * When a message is complete, the message callback is invoked.
     *
     * @param chunk The chunk data to process
     * @return AssemblyResult indicating success or failure
     */
    virtual AssemblyResult processChunk(const ChunkData& chunk) = 0;

    /**
     * @brief Abort a partial message on the specified chunk stream.
     *
     * Discards any buffered partial message data.
     *
     * @param chunkStreamId Chunk stream ID to abort
     */
    virtual void abortMessage(uint32_t chunkStreamId) = 0;

    /**
     * @brief Clear all pending messages and state.
     */
    virtual void clear() = 0;

    /**
     * @brief Get the number of pending partial messages.
     *
     * @return Number of chunk streams with partial message data
     */
    virtual size_t getPendingMessageCount() const = 0;

    /**
     * @brief Set the callback for completed messages.
     *
     * @param callback Function to call when a message is complete
     */
    virtual void setMessageCallback(MessageCallback callback) = 0;

    /**
     * @brief Set the callback for unknown message types.
     *
     * Used for logging unknown message types per Requirement 2.6.
     *
     * @param callback Function to call when an unknown type is encountered
     */
    virtual void setUnknownTypeCallback(UnknownTypeCallback callback) = 0;
};

/**
 * @brief Per-chunk-stream message assembly state.
 *
 * Tracks partial message data for each chunk stream.
 */
struct MessageAssemblyState {
    uint32_t expectedLength;              ///< Expected total message length
    uint32_t timestamp;                   ///< Message timestamp
    uint8_t messageTypeId;                ///< Message type ID
    uint32_t messageStreamId;             ///< Message stream ID
    std::vector<uint8_t> buffer;          ///< Accumulated payload data
    bool hasStarted;                      ///< Whether message assembly has started

    MessageAssemblyState()
        : expectedLength(0)
        , timestamp(0)
        , messageTypeId(0)
        , messageStreamId(0)
        , hasStarted(false) {}

    /**
     * @brief Reset the assembly state.
     */
    void reset() {
        expectedLength = 0;
        timestamp = 0;
        messageTypeId = 0;
        messageStreamId = 0;
        buffer.clear();
        hasStarted = false;
    }

    /**
     * @brief Get the number of bytes remaining to complete the message.
     *
     * @return Remaining bytes needed
     */
    [[nodiscard]] size_t remainingBytes() const {
        return (expectedLength > buffer.size()) ?
            (expectedLength - buffer.size()) : 0;
    }

    /**
     * @brief Check if the message is complete.
     *
     * @return true if all expected bytes have been received
     */
    [[nodiscard]] bool isComplete() const {
        return hasStarted && buffer.size() >= expectedLength;
    }
};

/**
 * @brief RTMP Message Assembler implementation.
 *
 * Reassembles complete RTMP messages from ChunkParser output.
 *
 * Key responsibilities:
 * - Buffer partial message data across multiple chunks
 * - Reassemble complete messages before forwarding
 * - Support all standard RTMP message types
 * - Log unknown message types and continue processing
 * - Validate message integrity and length consistency
 *
 * Requirements coverage:
 * - Requirement 2.3: Reassemble complete messages from multiple chunks
 * - Requirement 2.4: Support all standard RTMP message types
 * - Requirement 2.6: Log unknown message types and continue processing
 */
class MessageAssembler : public IMessageAssembler {
public:
    /**
     * @brief Construct a new MessageAssembler.
     */
    MessageAssembler();

    /**
     * @brief Destructor.
     */
    ~MessageAssembler() override = default;

    // Non-copyable but movable
    MessageAssembler(const MessageAssembler&) = delete;
    MessageAssembler& operator=(const MessageAssembler&) = delete;
    MessageAssembler(MessageAssembler&&) noexcept = default;
    MessageAssembler& operator=(MessageAssembler&&) noexcept = default;

    // IMessageAssembler interface
    AssemblyResult processChunk(const ChunkData& chunk) override;
    void abortMessage(uint32_t chunkStreamId) override;
    void clear() override;
    size_t getPendingMessageCount() const override;
    void setMessageCallback(MessageCallback callback) override;
    void setUnknownTypeCallback(UnknownTypeCallback callback) override;

private:
    /**
     * @brief Categorize a message type ID into MessageType enum.
     *
     * @param typeId Message type ID
     * @return MessageType category
     */
    MessageType categorizeMessageType(uint8_t typeId) const;

    /**
     * @brief Check if a message type ID is known.
     *
     * @param typeId Message type ID
     * @return true if the type is a known RTMP message type
     */
    bool isKnownMessageType(uint8_t typeId) const;

    /**
     * @brief Complete a message and invoke the callback.
     *
     * @param chunkStreamId Chunk stream ID
     * @param state The assembly state containing the complete message
     */
    void completeMessage(uint32_t chunkStreamId, const MessageAssemblyState& state);

    /**
     * @brief Get or create assembly state for a chunk stream.
     *
     * @param chunkStreamId Chunk stream ID
     * @return Reference to the assembly state
     */
    MessageAssemblyState& getState(uint32_t chunkStreamId);

    /// Per-chunk-stream assembly state
    std::unordered_map<uint32_t, MessageAssemblyState> states_;

    /// Callback for completed messages
    MessageCallback messageCallback_;

    /// Callback for unknown message types
    UnknownTypeCallback unknownTypeCallback_;
};

} // namespace protocol
} // namespace openrtmp

#endif // OPENRTMP_PROTOCOL_MESSAGE_ASSEMBLER_HPP
