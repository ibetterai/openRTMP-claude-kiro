// OpenRTMP - Cross-platform RTMP Server
// RTMP Chunk Stream Parser Implementation
//
// Implements RTMP chunk parsing per Adobe specification:
// - Basic Header parsing for chunk stream ID and format type
// - Message Header parsing for all 4 types (0-3)
// - Chunk size handling from 128 to 65536 bytes
// - Per-chunk-stream state for header compression
// - Set Chunk Size protocol control message handling
// - Abort Message processing
//
// Requirements coverage:
// - Requirement 2.1: Support chunk sizes from 128 to 65536 bytes
// - Requirement 2.2: Handle Set Chunk Size message
// - Requirement 2.5: Handle Abort Message to discard partial messages

#include "openrtmp/protocol/chunk_parser.hpp"
#include <algorithm>
#include <cstring>

namespace openrtmp {
namespace protocol {

// =============================================================================
// Constructor
// =============================================================================

ChunkParser::ChunkParser()
    : chunkSize_(chunk::DEFAULT_CHUNK_SIZE)
{
}

// =============================================================================
// IChunkParser Interface Implementation
// =============================================================================

ParseResult ChunkParser::parse(const uint8_t* data, size_t length) {
    if (data == nullptr || length == 0) {
        return ParseResult::ok(0);
    }

    size_t totalConsumed = 0;
    size_t chunksCompleted = 0;

    while (totalConsumed < length) {
        const uint8_t* current = data + totalConsumed;
        size_t remaining = length - totalConsumed;

        // Parse basic header
        uint8_t fmt = 0;
        uint32_t csid = 0;
        size_t basicHeaderSize = 0;

        if (!parseBasicHeader(current, remaining, fmt, csid, basicHeaderSize)) {
            // Need more data for basic header
            break;
        }

        // Get or create chunk stream state
        ChunkStreamState& state = getChunkStreamState(csid);

        // For Type 3 on uninitialized stream, we need prior state
        if (fmt == 3 && !hasChunkStreamState(csid)) {
            // This is an error - Type 3 requires prior state
            return ParseResult::fail(ParseError(
                ParseError::Code::UnknownChunkStream,
                "Type 3 chunk on uninitialized stream"));
        }

        // Parse message header
        size_t messageHeaderSize = 0;
        if (!parseMessageHeader(fmt, current + basicHeaderSize, remaining - basicHeaderSize,
                                state, messageHeaderSize)) {
            // Need more data for message header
            break;
        }

        size_t headerSize = basicHeaderSize + messageHeaderSize;

        // Check for extended timestamp
        size_t extendedTimestampSize = 0;
        if (state.hasExtendedTimestamp) {
            if (remaining < headerSize + 4) {
                // Need more data for extended timestamp
                break;
            }
            extendedTimestampSize = 4;

            // Read extended timestamp (big-endian)
            const uint8_t* extTs = current + headerSize;
            uint32_t extendedTimestamp =
                (static_cast<uint32_t>(extTs[0]) << 24) |
                (static_cast<uint32_t>(extTs[1]) << 16) |
                (static_cast<uint32_t>(extTs[2]) << 8) |
                static_cast<uint32_t>(extTs[3]);

            // Apply extended timestamp based on format type
            if (fmt == 0) {
                state.timestamp = extendedTimestamp;
            } else {
                // For Type 1/2/3 with extended timestamp, the delta is in extended field
                state.timestampDelta = extendedTimestamp;
                // We need to add the delta to the previous timestamp
                // Note: parseMessageHeader didn't add the delta when extended timestamp marker was set
                state.timestamp += extendedTimestamp;
            }
        }

        size_t totalHeaderSize = headerSize + extendedTimestampSize;

        // Calculate how much payload we can read in this chunk
        uint32_t chunkPayloadSize = chunkSize_;

        // For continuation chunks (not first chunk of message), state.remainingBytes tells us how much is left
        // For first chunk, we need to calculate based on messageLength
        if (state.partialPayload.empty() && state.remainingBytes == 0) {
            // First chunk of a new message
            state.remainingBytes = state.messageLength;
        }

        // Payload to read in this chunk is min(chunkSize, remainingBytes)
        uint32_t payloadToRead = std::min(chunkPayloadSize, state.remainingBytes);

        // Check if we have enough data for payload
        if (remaining < totalHeaderSize + payloadToRead) {
            // Partial payload available
            size_t availablePayload = remaining - totalHeaderSize;
            if (availablePayload > 0 && availablePayload < payloadToRead) {
                // Consume what we can
                state.partialPayload.insert(
                    state.partialPayload.end(),
                    current + totalHeaderSize,
                    current + totalHeaderSize + availablePayload
                );
                state.remainingBytes -= static_cast<uint32_t>(availablePayload);
                totalConsumed += totalHeaderSize + availablePayload;
                continue;
            }
            // Can't even start reading payload, need more data
            break;
        }

        // Read full chunk payload
        state.partialPayload.insert(
            state.partialPayload.end(),
            current + totalHeaderSize,
            current + totalHeaderSize + payloadToRead
        );
        state.remainingBytes -= payloadToRead;
        totalConsumed += totalHeaderSize + payloadToRead;

        // Check if message is complete
        if (state.remainingBytes == 0) {
            // Create completed chunk
            ChunkData chunk;
            chunk.chunkStreamId = csid;
            chunk.timestamp = state.timestamp;
            chunk.messageLength = state.messageLength;
            chunk.messageTypeId = state.messageTypeId;
            chunk.messageStreamId = state.messageStreamId;
            chunk.payload = std::move(state.partialPayload);
            chunk.isComplete = true;

            // Reset partial state
            state.partialPayload.clear();

            // Process protocol control messages
            processProtocolControlMessage(chunk);

            // Add to completed list
            completedChunks_.push_back(std::move(chunk));
            chunksCompleted++;
        }
    }

    return ParseResult::ok(totalConsumed, chunksCompleted);
}

void ChunkParser::setChunkSize(uint32_t size) {
    // Clamp to valid range
    if (size < chunk::MIN_CHUNK_SIZE) {
        size = chunk::MIN_CHUNK_SIZE;
    } else if (size > chunk::MAX_CHUNK_SIZE) {
        size = chunk::MAX_CHUNK_SIZE;
    }
    chunkSize_ = size;
}

void ChunkParser::abortChunkStream(uint32_t chunkStreamId) {
    auto it = chunkStreams_.find(chunkStreamId);
    if (it != chunkStreams_.end()) {
        it->second.resetPartial();
    }
}

std::vector<ChunkData> ChunkParser::getCompletedChunks() {
    std::vector<ChunkData> result = std::move(completedChunks_);
    completedChunks_.clear();
    return result;
}

// =============================================================================
// Private Helper Methods
// =============================================================================

bool ChunkParser::parseBasicHeader(
    const uint8_t* data,
    size_t length,
    uint8_t& fmt,
    uint32_t& csid,
    size_t& headerSize
) {
    if (length < 1) {
        return false;
    }

    uint8_t firstByte = data[0];
    fmt = (firstByte >> 6) & 0x03;  // Top 2 bits
    uint8_t csidField = firstByte & 0x3F;  // Bottom 6 bits

    if (csidField == 0) {
        // 2-byte header: csid = data[1] + 64
        if (length < 2) {
            return false;
        }
        csid = static_cast<uint32_t>(data[1]) + 64;
        headerSize = 2;
    } else if (csidField == 1) {
        // 3-byte header: csid = (data[2] << 8) + data[1] + 64
        if (length < 3) {
            return false;
        }
        csid = (static_cast<uint32_t>(data[2]) << 8) + static_cast<uint32_t>(data[1]) + 64;
        headerSize = 3;
    } else {
        // 1-byte header: csid = csidField (2-63)
        csid = csidField;
        headerSize = 1;
    }

    return true;
}

bool ChunkParser::parseMessageHeader(
    uint8_t fmt,
    const uint8_t* data,
    size_t length,
    ChunkStreamState& state,
    size_t& headerSize
) {
    switch (fmt) {
        case 0: {
            // Type 0: Full header (11 bytes)
            // timestamp (3) + message length (3) + message type id (1) + message stream id (4)
            if (length < 11) {
                return false;
            }

            // Timestamp (3 bytes, big-endian)
            uint32_t timestamp =
                (static_cast<uint32_t>(data[0]) << 16) |
                (static_cast<uint32_t>(data[1]) << 8) |
                static_cast<uint32_t>(data[2]);

            // Check for extended timestamp
            state.hasExtendedTimestamp = (timestamp == chunk::EXTENDED_TIMESTAMP_MARKER);
            if (!state.hasExtendedTimestamp) {
                state.timestamp = timestamp;
            }
            state.timestampDelta = 0;

            // Message length (3 bytes, big-endian)
            state.messageLength =
                (static_cast<uint32_t>(data[3]) << 16) |
                (static_cast<uint32_t>(data[4]) << 8) |
                static_cast<uint32_t>(data[5]);

            // Message type ID (1 byte)
            state.messageTypeId = data[6];

            // Message stream ID (4 bytes, little-endian)
            state.messageStreamId =
                static_cast<uint32_t>(data[7]) |
                (static_cast<uint32_t>(data[8]) << 8) |
                (static_cast<uint32_t>(data[9]) << 16) |
                (static_cast<uint32_t>(data[10]) << 24);

            headerSize = 11;
            break;
        }

        case 1: {
            // Type 1: 7 bytes (timestamp delta + message length + message type id)
            if (length < 7) {
                return false;
            }

            // Timestamp delta (3 bytes, big-endian)
            uint32_t timestampDelta =
                (static_cast<uint32_t>(data[0]) << 16) |
                (static_cast<uint32_t>(data[1]) << 8) |
                static_cast<uint32_t>(data[2]);

            state.hasExtendedTimestamp = (timestampDelta == chunk::EXTENDED_TIMESTAMP_MARKER);
            if (!state.hasExtendedTimestamp) {
                state.timestampDelta = timestampDelta;
                state.timestamp += timestampDelta;
            }

            // Message length (3 bytes, big-endian)
            state.messageLength =
                (static_cast<uint32_t>(data[3]) << 16) |
                (static_cast<uint32_t>(data[4]) << 8) |
                static_cast<uint32_t>(data[5]);

            // Message type ID (1 byte)
            state.messageTypeId = data[6];

            // Message stream ID is inherited from previous

            headerSize = 7;
            break;
        }

        case 2: {
            // Type 2: 3 bytes (timestamp delta only)
            if (length < 3) {
                return false;
            }

            // Timestamp delta (3 bytes, big-endian)
            uint32_t timestampDelta =
                (static_cast<uint32_t>(data[0]) << 16) |
                (static_cast<uint32_t>(data[1]) << 8) |
                static_cast<uint32_t>(data[2]);

            state.hasExtendedTimestamp = (timestampDelta == chunk::EXTENDED_TIMESTAMP_MARKER);
            if (!state.hasExtendedTimestamp) {
                state.timestampDelta = timestampDelta;
                state.timestamp += timestampDelta;
            }

            // Message length, type ID, and stream ID are inherited

            headerSize = 3;
            break;
        }

        case 3: {
            // Type 3: No message header (0 bytes)
            // All values inherited from previous chunk on this stream

            // If previous chunk had extended timestamp, we still need to read it
            // But we check for this in the caller based on hasExtendedTimestamp

            headerSize = 0;
            break;
        }

        default:
            return false;
    }

    return true;
}

void ChunkParser::processProtocolControlMessage(const ChunkData& chunk) {
    if (chunk.messageStreamId != 0) {
        // Protocol control messages are on message stream 0
        return;
    }

    switch (chunk.messageTypeId) {
        case chunk::MSG_TYPE_SET_CHUNK_SIZE: {
            // Set Chunk Size (4 bytes, big-endian, MSB must be 0)
            if (chunk.payload.size() >= 4) {
                uint32_t newSize =
                    ((static_cast<uint32_t>(chunk.payload[0]) & 0x7F) << 24) |
                    (static_cast<uint32_t>(chunk.payload[1]) << 16) |
                    (static_cast<uint32_t>(chunk.payload[2]) << 8) |
                    static_cast<uint32_t>(chunk.payload[3]);
                setChunkSize(newSize);
            }
            break;
        }

        case chunk::MSG_TYPE_ABORT: {
            // Abort Message (4 bytes, big-endian chunk stream id)
            if (chunk.payload.size() >= 4) {
                uint32_t csidToAbort =
                    (static_cast<uint32_t>(chunk.payload[0]) << 24) |
                    (static_cast<uint32_t>(chunk.payload[1]) << 16) |
                    (static_cast<uint32_t>(chunk.payload[2]) << 8) |
                    static_cast<uint32_t>(chunk.payload[3]);
                abortChunkStream(csidToAbort);
            }
            break;
        }

        // Other protocol control messages (Acknowledgement, User Control, etc.)
        // are passed through to higher layers
        default:
            break;
    }
}

ChunkStreamState& ChunkParser::getChunkStreamState(uint32_t csid) {
    return chunkStreams_[csid];  // Creates if doesn't exist
}

bool ChunkParser::hasChunkStreamState(uint32_t csid) const {
    return chunkStreams_.find(csid) != chunkStreams_.end();
}

} // namespace protocol
} // namespace openrtmp
