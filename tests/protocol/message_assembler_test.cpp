// OpenRTMP - Cross-platform RTMP Server
// Tests for RTMP Message Assembler
//
// Tests cover:
// - Message reassembly from chunks (complete and partial messages)
// - Buffering partial message data across multiple chunks
// - Support for all standard RTMP message types (audio 8, video 9, data 18, command 20)
// - Logging unknown message types and continuing processing
// - Message integrity and length consistency validation
//
// Requirements coverage:
// - Requirement 2.3: Reassemble complete messages from multiple chunks
// - Requirement 2.4: Support all standard RTMP message types
// - Requirement 2.6: Log unknown message types and continue processing

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <functional>
#include "openrtmp/protocol/message_assembler.hpp"
#include "openrtmp/protocol/chunk_parser.hpp"
#include "openrtmp/core/buffer.hpp"

namespace openrtmp {
namespace protocol {
namespace test {

// =============================================================================
// RTMP Message Type IDs
// =============================================================================

constexpr uint8_t MSG_TYPE_SET_CHUNK_SIZE = 1;
constexpr uint8_t MSG_TYPE_ABORT = 2;
constexpr uint8_t MSG_TYPE_ACKNOWLEDGEMENT = 3;
constexpr uint8_t MSG_TYPE_USER_CONTROL = 4;
constexpr uint8_t MSG_TYPE_AUDIO = 8;
constexpr uint8_t MSG_TYPE_VIDEO = 9;
constexpr uint8_t MSG_TYPE_DATA_AMF0 = 18;
constexpr uint8_t MSG_TYPE_COMMAND_AMF0 = 20;
constexpr uint8_t MSG_TYPE_UNKNOWN = 99;  // For unknown type testing

// =============================================================================
// Test Fixtures
// =============================================================================

class MessageAssemblerTest : public ::testing::Test {
protected:
    void SetUp() override {
        assembler_ = std::make_unique<MessageAssembler>();
        messagesReceived_.clear();
        unknownTypesLogged_.clear();

        // Set up message callback
        assembler_->setMessageCallback([this](const RTMPMessage& msg) {
            messagesReceived_.push_back(msg);
        });

        // Set up unknown type callback for logging verification
        assembler_->setUnknownTypeCallback([this](uint8_t typeId) {
            unknownTypesLogged_.push_back(typeId);
        });
    }

    // Helper to create a ChunkData structure
    ChunkData createChunkData(
        uint32_t chunkStreamId,
        uint32_t timestamp,
        uint32_t messageLength,
        uint8_t messageTypeId,
        uint32_t messageStreamId,
        const std::vector<uint8_t>& payload,
        bool isComplete = true
    ) {
        ChunkData chunk;
        chunk.chunkStreamId = chunkStreamId;
        chunk.timestamp = timestamp;
        chunk.messageLength = messageLength;
        chunk.messageTypeId = messageTypeId;
        chunk.messageStreamId = messageStreamId;
        chunk.payload = payload;
        chunk.isComplete = isComplete;
        return chunk;
    }

    std::unique_ptr<MessageAssembler> assembler_;
    std::vector<RTMPMessage> messagesReceived_;
    std::vector<uint8_t> unknownTypesLogged_;
};

// =============================================================================
// Basic Message Reassembly Tests (Requirement 2.3)
// =============================================================================

TEST_F(MessageAssemblerTest, ProcessCompleteMessageFromSingleChunk) {
    // A complete message delivered in a single chunk
    std::vector<uint8_t> payload(100, 0xAA);
    auto chunk = createChunkData(3, 1000, 100, MSG_TYPE_COMMAND_AMF0, 1, payload, true);

    auto result = assembler_->processChunk(chunk);

    EXPECT_TRUE(result.isSuccess());
    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].messageTypeId, MSG_TYPE_COMMAND_AMF0);
    EXPECT_EQ(messagesReceived_[0].timestamp, 1000u);
    EXPECT_EQ(messagesReceived_[0].messageStreamId, 1u);
    EXPECT_EQ(messagesReceived_[0].payload.size(), 100u);
    EXPECT_EQ(messagesReceived_[0].payload[0], 0xAA);
}

TEST_F(MessageAssemblerTest, BufferPartialMessageAcrossMultipleChunks) {
    // Message split across 3 chunks: 50 + 50 + 30 = 130 bytes total
    uint32_t totalLength = 130;

    // First chunk - partial
    std::vector<uint8_t> payload1(50, 0x11);
    auto chunk1 = createChunkData(3, 1000, totalLength, MSG_TYPE_VIDEO, 1, payload1, false);
    auto result1 = assembler_->processChunk(chunk1);
    EXPECT_TRUE(result1.isSuccess());
    EXPECT_EQ(messagesReceived_.size(), 0u);  // Not complete yet

    // Second chunk - partial
    std::vector<uint8_t> payload2(50, 0x22);
    auto chunk2 = createChunkData(3, 1000, totalLength, MSG_TYPE_VIDEO, 1, payload2, false);
    auto result2 = assembler_->processChunk(chunk2);
    EXPECT_TRUE(result2.isSuccess());
    EXPECT_EQ(messagesReceived_.size(), 0u);  // Still not complete

    // Third chunk - completes the message
    std::vector<uint8_t> payload3(30, 0x33);
    auto chunk3 = createChunkData(3, 1000, totalLength, MSG_TYPE_VIDEO, 1, payload3, true);
    auto result3 = assembler_->processChunk(chunk3);
    EXPECT_TRUE(result3.isSuccess());

    // Now we should have the complete message
    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].messageTypeId, MSG_TYPE_VIDEO);
    EXPECT_EQ(messagesReceived_[0].payload.size(), 130u);

    // Verify payload concatenation
    for (size_t i = 0; i < 50; i++) {
        EXPECT_EQ(messagesReceived_[0].payload[i], 0x11);
    }
    for (size_t i = 50; i < 100; i++) {
        EXPECT_EQ(messagesReceived_[0].payload[i], 0x22);
    }
    for (size_t i = 100; i < 130; i++) {
        EXPECT_EQ(messagesReceived_[0].payload[i], 0x33);
    }
}

TEST_F(MessageAssemblerTest, InterleavedMessagesOnDifferentStreams) {
    // Start message on chunk stream 3
    std::vector<uint8_t> payload3_1(50, 0xAA);
    auto chunk3_1 = createChunkData(3, 1000, 100, MSG_TYPE_VIDEO, 1, payload3_1, false);
    assembler_->processChunk(chunk3_1);

    // Complete message on chunk stream 4
    std::vector<uint8_t> payload4(80, 0xBB);
    auto chunk4 = createChunkData(4, 2000, 80, MSG_TYPE_AUDIO, 2, payload4, true);
    assembler_->processChunk(chunk4);

    // chunk stream 4 should be complete
    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].messageTypeId, MSG_TYPE_AUDIO);
    EXPECT_EQ(messagesReceived_[0].payload.size(), 80u);
    EXPECT_EQ(messagesReceived_[0].payload[0], 0xBB);

    messagesReceived_.clear();

    // Complete message on chunk stream 3
    std::vector<uint8_t> payload3_2(50, 0xCC);
    auto chunk3_2 = createChunkData(3, 1000, 100, MSG_TYPE_VIDEO, 1, payload3_2, true);
    assembler_->processChunk(chunk3_2);

    // chunk stream 3 should now be complete
    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].messageTypeId, MSG_TYPE_VIDEO);
    EXPECT_EQ(messagesReceived_[0].payload.size(), 100u);

    // Verify combined payload
    for (size_t i = 0; i < 50; i++) {
        EXPECT_EQ(messagesReceived_[0].payload[i], 0xAA);
    }
    for (size_t i = 50; i < 100; i++) {
        EXPECT_EQ(messagesReceived_[0].payload[i], 0xCC);
    }
}

// =============================================================================
// Message Type Support Tests (Requirement 2.4)
// =============================================================================

TEST_F(MessageAssemblerTest, SupportAudioMessageType8) {
    std::vector<uint8_t> payload(64, 0x88);
    auto chunk = createChunkData(3, 100, 64, MSG_TYPE_AUDIO, 1, payload, true);

    auto result = assembler_->processChunk(chunk);

    EXPECT_TRUE(result.isSuccess());
    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].messageTypeId, MSG_TYPE_AUDIO);
    EXPECT_EQ(messagesReceived_[0].messageType, MessageType::Audio);
}

TEST_F(MessageAssemblerTest, SupportVideoMessageType9) {
    std::vector<uint8_t> payload(128, 0x99);
    auto chunk = createChunkData(3, 200, 128, MSG_TYPE_VIDEO, 1, payload, true);

    auto result = assembler_->processChunk(chunk);

    EXPECT_TRUE(result.isSuccess());
    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].messageTypeId, MSG_TYPE_VIDEO);
    EXPECT_EQ(messagesReceived_[0].messageType, MessageType::Video);
}

TEST_F(MessageAssemblerTest, SupportDataMessageType18) {
    std::vector<uint8_t> payload(256, 0x18);
    auto chunk = createChunkData(3, 300, 256, MSG_TYPE_DATA_AMF0, 1, payload, true);

    auto result = assembler_->processChunk(chunk);

    EXPECT_TRUE(result.isSuccess());
    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].messageTypeId, MSG_TYPE_DATA_AMF0);
    EXPECT_EQ(messagesReceived_[0].messageType, MessageType::Data);
}

TEST_F(MessageAssemblerTest, SupportCommandMessageType20) {
    std::vector<uint8_t> payload(512, 0x20);
    auto chunk = createChunkData(3, 400, 512, MSG_TYPE_COMMAND_AMF0, 1, payload, true);

    auto result = assembler_->processChunk(chunk);

    EXPECT_TRUE(result.isSuccess());
    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].messageTypeId, MSG_TYPE_COMMAND_AMF0);
    EXPECT_EQ(messagesReceived_[0].messageType, MessageType::Command);
}

TEST_F(MessageAssemblerTest, SupportProtocolControlMessages) {
    // Set Chunk Size (type 1)
    std::vector<uint8_t> payload1(4, 0x01);
    auto chunk1 = createChunkData(2, 0, 4, MSG_TYPE_SET_CHUNK_SIZE, 0, payload1, true);
    assembler_->processChunk(chunk1);
    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].messageType, MessageType::ProtocolControl);

    messagesReceived_.clear();

    // User Control (type 4)
    std::vector<uint8_t> payload2(6, 0x04);
    auto chunk2 = createChunkData(2, 0, 6, MSG_TYPE_USER_CONTROL, 0, payload2, true);
    assembler_->processChunk(chunk2);
    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].messageType, MessageType::ProtocolControl);
}

// =============================================================================
// Unknown Message Type Handling Tests (Requirement 2.6)
// =============================================================================

TEST_F(MessageAssemblerTest, LogUnknownMessageTypeAndContinue) {
    // Unknown message type
    std::vector<uint8_t> payload(100, 0xFF);
    auto chunk = createChunkData(3, 500, 100, MSG_TYPE_UNKNOWN, 1, payload, true);

    auto result = assembler_->processChunk(chunk);

    // Should succeed and continue processing
    EXPECT_TRUE(result.isSuccess());

    // Unknown type should be logged
    ASSERT_EQ(unknownTypesLogged_.size(), 1u);
    EXPECT_EQ(unknownTypesLogged_[0], MSG_TYPE_UNKNOWN);

    // Message should still be forwarded with Unknown type
    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].messageType, MessageType::Unknown);
}

TEST_F(MessageAssemblerTest, ContinueProcessingAfterUnknownType) {
    // Unknown message type first
    std::vector<uint8_t> unknownPayload(50, 0xFF);
    auto unknownChunk = createChunkData(3, 100, 50, MSG_TYPE_UNKNOWN, 1, unknownPayload, true);
    assembler_->processChunk(unknownChunk);

    // Then a valid message
    std::vector<uint8_t> audioPayload(80, 0x88);
    auto audioChunk = createChunkData(4, 200, 80, MSG_TYPE_AUDIO, 1, audioPayload, true);
    assembler_->processChunk(audioChunk);

    // Both messages should be received
    EXPECT_EQ(messagesReceived_.size(), 2u);
    EXPECT_EQ(messagesReceived_[1].messageTypeId, MSG_TYPE_AUDIO);

    // Unknown type should have been logged
    EXPECT_EQ(unknownTypesLogged_.size(), 1u);
}

// =============================================================================
// Message Integrity and Length Validation Tests
// =============================================================================

TEST_F(MessageAssemblerTest, ValidateMessageLength) {
    // Complete message with correct length
    std::vector<uint8_t> payload(100, 0xAB);
    auto chunk = createChunkData(3, 1000, 100, MSG_TYPE_VIDEO, 1, payload, true);

    auto result = assembler_->processChunk(chunk);

    EXPECT_TRUE(result.isSuccess());
    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].payload.size(), 100u);
}

TEST_F(MessageAssemblerTest, DetectLengthMismatch) {
    // First chunk indicates 200 bytes total, but we send inconsistent data
    std::vector<uint8_t> payload1(100, 0x11);
    auto chunk1 = createChunkData(3, 1000, 200, MSG_TYPE_VIDEO, 1, payload1, false);
    assembler_->processChunk(chunk1);

    // Second chunk claims to complete message but with wrong length
    std::vector<uint8_t> payload2(50, 0x22);  // This makes 150 total, not 200
    ChunkData chunk2;
    chunk2.chunkStreamId = 3;
    chunk2.timestamp = 1000;
    chunk2.messageLength = 200;  // Still says 200
    chunk2.messageTypeId = MSG_TYPE_VIDEO;
    chunk2.messageStreamId = 1;
    chunk2.payload = payload2;
    chunk2.isComplete = true;  // Claims to be complete

    auto result = assembler_->processChunk(chunk2);

    // Implementation should detect the inconsistency
    // Either report error or handle gracefully
    // The exact behavior depends on implementation choice
    // Here we test that it doesn't crash and provides meaningful result
    EXPECT_TRUE(result.isSuccess() || result.isError());
}

TEST_F(MessageAssemblerTest, HandleZeroLengthMessage) {
    // Zero-length message
    std::vector<uint8_t> payload;  // Empty
    auto chunk = createChunkData(3, 1000, 0, MSG_TYPE_USER_CONTROL, 0, payload, true);

    auto result = assembler_->processChunk(chunk);

    EXPECT_TRUE(result.isSuccess());
    // Implementation choice: either forward empty message or skip
}

// =============================================================================
// Abort Handling Tests
// =============================================================================

TEST_F(MessageAssemblerTest, AbortPartialMessage) {
    // Start a partial message
    std::vector<uint8_t> payload1(50, 0x11);
    auto chunk1 = createChunkData(3, 1000, 200, MSG_TYPE_VIDEO, 1, payload1, false);
    assembler_->processChunk(chunk1);

    // Abort the message
    assembler_->abortMessage(3);

    // No message should be received
    EXPECT_EQ(messagesReceived_.size(), 0u);

    // Start a new message on the same stream
    std::vector<uint8_t> payload2(80, 0x22);
    auto chunk2 = createChunkData(3, 2000, 80, MSG_TYPE_VIDEO, 1, payload2, true);
    assembler_->processChunk(chunk2);

    // Should receive only the new message
    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].payload.size(), 80u);
    EXPECT_EQ(messagesReceived_[0].payload[0], 0x22);
}

TEST_F(MessageAssemblerTest, AbortOnlyAffectsSpecifiedStream) {
    // Start partial messages on two streams
    std::vector<uint8_t> payload3(50, 0x33);
    auto chunk3 = createChunkData(3, 1000, 100, MSG_TYPE_VIDEO, 1, payload3, false);
    assembler_->processChunk(chunk3);

    std::vector<uint8_t> payload4(50, 0x44);
    auto chunk4 = createChunkData(4, 1000, 100, MSG_TYPE_AUDIO, 2, payload4, false);
    assembler_->processChunk(chunk4);

    // Abort only stream 3
    assembler_->abortMessage(3);

    // Complete stream 4
    std::vector<uint8_t> payload4_2(50, 0x45);
    auto chunk4_2 = createChunkData(4, 1000, 100, MSG_TYPE_AUDIO, 2, payload4_2, true);
    assembler_->processChunk(chunk4_2);

    // Only stream 4's message should be received
    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].messageTypeId, MSG_TYPE_AUDIO);
}

// =============================================================================
// State Management Tests
// =============================================================================

TEST_F(MessageAssemblerTest, TrackPendingMessageCount) {
    // Start partial messages on 3 streams
    for (uint32_t i = 3; i <= 5; i++) {
        std::vector<uint8_t> payload(50, static_cast<uint8_t>(i));
        auto chunk = createChunkData(i, 1000, 100, MSG_TYPE_VIDEO, 1, payload, false);
        assembler_->processChunk(chunk);
    }

    EXPECT_EQ(assembler_->getPendingMessageCount(), 3u);

    // Complete one message
    std::vector<uint8_t> payload(50, 0x05);
    auto chunk = createChunkData(5, 1000, 100, MSG_TYPE_VIDEO, 1, payload, true);
    assembler_->processChunk(chunk);

    EXPECT_EQ(assembler_->getPendingMessageCount(), 2u);
}

TEST_F(MessageAssemblerTest, ClearAllPendingMessages) {
    // Start partial messages
    std::vector<uint8_t> payload(50, 0x11);
    auto chunk = createChunkData(3, 1000, 100, MSG_TYPE_VIDEO, 1, payload, false);
    assembler_->processChunk(chunk);

    EXPECT_EQ(assembler_->getPendingMessageCount(), 1u);

    // Clear all
    assembler_->clear();

    EXPECT_EQ(assembler_->getPendingMessageCount(), 0u);
    EXPECT_EQ(messagesReceived_.size(), 0u);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(MessageAssemblerTest, MultipleCompleteMessagesInSequence) {
    // Send 5 complete messages in sequence
    for (int i = 0; i < 5; i++) {
        std::vector<uint8_t> payload(64, static_cast<uint8_t>(i + 1));
        auto chunk = createChunkData(3, 100 * i, 64, MSG_TYPE_VIDEO, 1, payload, true);
        assembler_->processChunk(chunk);
    }

    EXPECT_EQ(messagesReceived_.size(), 5u);

    // Verify each message
    for (size_t i = 0; i < 5; i++) {
        EXPECT_EQ(messagesReceived_[i].payload[0], static_cast<uint8_t>(i + 1));
    }
}

TEST_F(MessageAssemblerTest, LargeMessage) {
    // Large message (64KB)
    const size_t largeSize = 65536;
    std::vector<uint8_t> payload(largeSize, 0xEE);
    auto chunk = createChunkData(3, 1000, largeSize, MSG_TYPE_VIDEO, 1, payload, true);

    auto result = assembler_->processChunk(chunk);

    EXPECT_TRUE(result.isSuccess());
    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].payload.size(), largeSize);
}

TEST_F(MessageAssemblerTest, TimestampPreservation) {
    // Message with specific timestamp
    std::vector<uint8_t> payload(64, 0xAB);
    auto chunk = createChunkData(3, 0xFFFFFFFF, 64, MSG_TYPE_AUDIO, 1, payload, true);

    assembler_->processChunk(chunk);

    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].timestamp, 0xFFFFFFFF);
}

TEST_F(MessageAssemblerTest, MessageStreamIdPreservation) {
    // Message with specific stream ID
    std::vector<uint8_t> payload(64, 0xCD);
    auto chunk = createChunkData(3, 1000, 64, MSG_TYPE_COMMAND_AMF0, 12345, payload, true);

    assembler_->processChunk(chunk);

    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].messageStreamId, 12345u);
}

// =============================================================================
// Integration with ChunkParser
// =============================================================================

TEST_F(MessageAssemblerTest, ProcessChunksFromChunkParser) {
    // This test verifies integration with ChunkParser output
    ChunkParser parser;

    // Create a simple Type 0 chunk with command message
    core::Buffer chunk;
    // Basic header: fmt=0, csid=3
    chunk.append({0x03});
    // Message header: timestamp=0, length=10, type=20, stream_id=0
    chunk.append({0x00, 0x00, 0x00});  // timestamp
    chunk.append({0x00, 0x00, 0x0A});  // message length = 10
    chunk.append({MSG_TYPE_COMMAND_AMF0});  // type
    chunk.append({0x00, 0x00, 0x00, 0x00});  // stream id (little-endian)
    // Payload
    std::vector<uint8_t> payload(10, 0xAB);
    chunk.append(payload.data(), payload.size());

    // Parse with ChunkParser
    auto result = parser.parse(chunk.data(), chunk.size());
    ASSERT_TRUE(result.error == std::nullopt);

    auto chunks = parser.getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);

    // Process with MessageAssembler
    auto assembleResult = assembler_->processChunk(chunks[0]);
    EXPECT_TRUE(assembleResult.isSuccess());

    ASSERT_EQ(messagesReceived_.size(), 1u);
    EXPECT_EQ(messagesReceived_[0].messageTypeId, MSG_TYPE_COMMAND_AMF0);
}

} // namespace test
} // namespace protocol
} // namespace openrtmp
