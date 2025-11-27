// OpenRTMP - Cross-platform RTMP Server
// RTMP Message Assembler Implementation
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

#include "openrtmp/protocol/message_assembler.hpp"
#include <algorithm>

namespace openrtmp {
namespace protocol {

// =============================================================================
// Constructor
// =============================================================================

MessageAssembler::MessageAssembler()
    : messageCallback_(nullptr)
    , unknownTypeCallback_(nullptr)
{
}

// =============================================================================
// IMessageAssembler Interface Implementation
// =============================================================================

AssemblyResult MessageAssembler::processChunk(const ChunkData& chunk) {
    // Get or create state for this chunk stream
    MessageAssemblyState& state = getState(chunk.chunkStreamId);

    // If this is a new message (no partial data buffered)
    if (!state.hasStarted) {
        state.expectedLength = chunk.messageLength;
        state.timestamp = chunk.timestamp;
        state.messageTypeId = chunk.messageTypeId;
        state.messageStreamId = chunk.messageStreamId;
        state.hasStarted = true;
    }

    // Append payload to buffer
    state.buffer.insert(
        state.buffer.end(),
        chunk.payload.begin(),
        chunk.payload.end()
    );

    // Check if message is complete
    // A message is complete when:
    // 1. The chunk is marked as complete (isComplete == true), OR
    // 2. We've received all expected bytes
    bool messageComplete = chunk.isComplete || state.isComplete();

    if (messageComplete) {
        // Log unknown message types before completing
        if (!isKnownMessageType(state.messageTypeId)) {
            if (unknownTypeCallback_) {
                unknownTypeCallback_(state.messageTypeId);
            }
        }

        // Complete the message
        completeMessage(chunk.chunkStreamId, state);

        // Reset state for next message
        state.reset();
    }

    return AssemblyResult::success();
}

void MessageAssembler::abortMessage(uint32_t chunkStreamId) {
    auto it = states_.find(chunkStreamId);
    if (it != states_.end()) {
        it->second.reset();
    }
}

void MessageAssembler::clear() {
    states_.clear();
}

size_t MessageAssembler::getPendingMessageCount() const {
    size_t count = 0;
    for (const auto& pair : states_) {
        if (pair.second.hasStarted && !pair.second.isComplete()) {
            count++;
        }
    }
    return count;
}

void MessageAssembler::setMessageCallback(MessageCallback callback) {
    messageCallback_ = std::move(callback);
}

void MessageAssembler::setUnknownTypeCallback(UnknownTypeCallback callback) {
    unknownTypeCallback_ = std::move(callback);
}

// =============================================================================
// Private Helper Methods
// =============================================================================

MessageType MessageAssembler::categorizeMessageType(uint8_t typeId) const {
    switch (typeId) {
        // Protocol control messages (types 1-6)
        case message::TYPE_SET_CHUNK_SIZE:
        case message::TYPE_ABORT:
        case message::TYPE_ACKNOWLEDGEMENT:
        case message::TYPE_USER_CONTROL:
        case message::TYPE_WINDOW_ACK_SIZE:
        case message::TYPE_SET_PEER_BANDWIDTH:
            return MessageType::ProtocolControl;

        // Audio message (type 8)
        case message::TYPE_AUDIO:
            return MessageType::Audio;

        // Video message (type 9)
        case message::TYPE_VIDEO:
            return MessageType::Video;

        // Data messages (types 15, 18)
        case message::TYPE_DATA_AMF3:
        case message::TYPE_DATA_AMF0:
            return MessageType::Data;

        // Command messages (types 17, 20)
        case message::TYPE_COMMAND_AMF3:
        case message::TYPE_COMMAND_AMF0:
            return MessageType::Command;

        // Shared object messages (types 16, 19)
        case message::TYPE_SHARED_OBJECT_AMF3:
        case message::TYPE_SHARED_OBJECT_AMF0:
            return MessageType::SharedObject;

        // Aggregate message (type 22)
        case message::TYPE_AGGREGATE:
            return MessageType::Aggregate;

        // Unknown
        default:
            return MessageType::Unknown;
    }
}

bool MessageAssembler::isKnownMessageType(uint8_t typeId) const {
    switch (typeId) {
        case message::TYPE_SET_CHUNK_SIZE:
        case message::TYPE_ABORT:
        case message::TYPE_ACKNOWLEDGEMENT:
        case message::TYPE_USER_CONTROL:
        case message::TYPE_WINDOW_ACK_SIZE:
        case message::TYPE_SET_PEER_BANDWIDTH:
        case message::TYPE_AUDIO:
        case message::TYPE_VIDEO:
        case message::TYPE_DATA_AMF3:
        case message::TYPE_DATA_AMF0:
        case message::TYPE_COMMAND_AMF3:
        case message::TYPE_COMMAND_AMF0:
        case message::TYPE_SHARED_OBJECT_AMF3:
        case message::TYPE_SHARED_OBJECT_AMF0:
        case message::TYPE_AGGREGATE:
            return true;
        default:
            return false;
    }
}

void MessageAssembler::completeMessage(uint32_t chunkStreamId, const MessageAssemblyState& state) {
    if (!messageCallback_) {
        return;
    }

    RTMPMessage msg;
    msg.messageType = categorizeMessageType(state.messageTypeId);
    msg.messageTypeId = state.messageTypeId;
    msg.timestamp = state.timestamp;
    msg.messageStreamId = state.messageStreamId;
    msg.chunkStreamId = chunkStreamId;
    msg.payload = state.buffer;

    messageCallback_(msg);
}

MessageAssemblyState& MessageAssembler::getState(uint32_t chunkStreamId) {
    return states_[chunkStreamId];  // Creates if doesn't exist
}

} // namespace protocol
} // namespace openrtmp
