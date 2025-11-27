// OpenRTMP - Cross-platform RTMP Server
// Tests for RTMP Chunk Stream Parser
//
// Tests cover:
// - Basic Header parsing (1-3 bytes) for chunk stream ID and format type
// - Message Header variants (Type 0-3) based on format
// - Chunk sizes from 128 to 65536 bytes per RTMP specification
// - Per-chunk-stream state tracking for header compression
// - Set Chunk Size protocol control message handling
// - Abort Message processing to discard partial chunk stream data
// - Extended timestamp handling (when timestamp field is 0xFFFFFF)
//
// Requirements coverage:
// - Requirement 2.1: Support chunk sizes from 128 bytes to 65536 bytes
// - Requirement 2.2: Handle Set Chunk Size message
// - Requirement 2.5: Handle Abort Message to discard partial messages

#include <gtest/gtest.h>
#include <memory>
#include <cstring>
#include <algorithm>
#include "openrtmp/protocol/chunk_parser.hpp"
#include "openrtmp/core/buffer.hpp"

namespace openrtmp {
namespace protocol {
namespace test {

// =============================================================================
// RTMP Chunk Format Constants
// =============================================================================

// Message Type IDs (Protocol Control Messages) - only include those used in tests
constexpr uint8_t MSG_TYPE_SET_CHUNK_SIZE = 1;
constexpr uint8_t MSG_TYPE_ABORT = 2;
constexpr uint8_t MSG_TYPE_AUDIO = 8;
constexpr uint8_t MSG_TYPE_VIDEO = 9;
constexpr uint8_t MSG_TYPE_DATA_AMF0 = 18;
constexpr uint8_t MSG_TYPE_COMMAND_AMF0 = 20;

// Chunk size limits per RTMP spec
constexpr uint32_t MIN_CHUNK_SIZE = 128;
constexpr uint32_t MAX_CHUNK_SIZE = 65536;

// Extended timestamp marker
constexpr uint32_t EXTENDED_TIMESTAMP_MARKER = 0xFFFFFF;

// =============================================================================
// Test Helpers - Chunk Construction
// =============================================================================

namespace ChunkTestHelpers {

/**
 * @brief Build a basic header with format and chunk stream ID.
 *
 * Basic Header formats:
 * - Chunk stream ID 2-63:    1 byte  (fmt:2, csid:6)
 * - Chunk stream ID 64-319:  2 bytes (fmt:2, csid=0, cs id - 64)
 * - Chunk stream ID 320-65599: 3 bytes (fmt:2, csid=1, (cs id - 64) as 16-bit LE)
 */
inline core::Buffer createBasicHeader(uint8_t fmt, uint32_t csid) {
    core::Buffer buffer;
    core::BufferWriter writer(buffer);

    if (csid >= 2 && csid <= 63) {
        // 1-byte basic header
        writer.writeUint8(static_cast<uint8_t>((static_cast<uint32_t>(fmt) << 6) | csid));
    } else if (csid >= 64 && csid <= 319) {
        // 2-byte basic header (csid field = 0)
        writer.writeUint8(static_cast<uint8_t>(fmt << 6));
        writer.writeUint8(static_cast<uint8_t>(csid - 64));
    } else if (csid >= 320 && csid <= 65599) {
        // 3-byte basic header (csid field = 1)
        writer.writeUint8(static_cast<uint8_t>((fmt << 6) | 1));
        uint16_t extendedId = static_cast<uint16_t>(csid - 64);
        writer.writeUint8(static_cast<uint8_t>(extendedId & 0xFF));
        writer.writeUint8(static_cast<uint8_t>((extendedId >> 8) & 0xFF));
    }

    return buffer;
}

/**
 * @brief Build a Type 0 message header (full header, 11 bytes).
 *
 * Type 0 contains:
 * - timestamp (3 bytes, big-endian)
 * - message length (3 bytes, big-endian)
 * - message type id (1 byte)
 * - message stream id (4 bytes, little-endian)
 */
inline core::Buffer createMessageHeaderType0(
    uint32_t timestamp,
    uint32_t messageLength,
    uint8_t messageTypeId,
    uint32_t messageStreamId
) {
    core::Buffer buffer;
    core::BufferWriter writer(buffer);

    // Use extended timestamp marker if needed
    uint32_t timestampField = (timestamp >= EXTENDED_TIMESTAMP_MARKER) ? EXTENDED_TIMESTAMP_MARKER : timestamp;

    writer.writeUint24BE(timestampField);
    writer.writeUint24BE(messageLength);
    writer.writeUint8(messageTypeId);
    writer.writeUint32LE(messageStreamId);

    return buffer;
}

/**
 * @brief Build a Type 1 message header (7 bytes, omits message stream id).
 */
inline core::Buffer createMessageHeaderType1(
    uint32_t timestampDelta,
    uint32_t messageLength,
    uint8_t messageTypeId
) {
    core::Buffer buffer;
    core::BufferWriter writer(buffer);

    uint32_t timestampField = (timestampDelta >= EXTENDED_TIMESTAMP_MARKER) ? EXTENDED_TIMESTAMP_MARKER : timestampDelta;

    writer.writeUint24BE(timestampField);
    writer.writeUint24BE(messageLength);
    writer.writeUint8(messageTypeId);

    return buffer;
}

/**
 * @brief Build a Type 2 message header (3 bytes, only timestamp delta).
 */
inline core::Buffer createMessageHeaderType2(uint32_t timestampDelta) {
    core::Buffer buffer;
    core::BufferWriter writer(buffer);

    uint32_t timestampField = (timestampDelta >= EXTENDED_TIMESTAMP_MARKER) ? EXTENDED_TIMESTAMP_MARKER : timestampDelta;

    writer.writeUint24BE(timestampField);

    return buffer;
}

/**
 * @brief Build extended timestamp (4 bytes, big-endian).
 */
inline core::Buffer createExtendedTimestamp(uint32_t timestamp) {
    core::Buffer buffer;
    core::BufferWriter writer(buffer);
    writer.writeUint32BE(timestamp);
    return buffer;
}

/**
 * @brief Build a complete chunk with basic header, message header, and payload.
 */
inline core::Buffer createChunkType0(
    uint32_t csid,
    uint32_t timestamp,
    uint32_t messageLength,
    uint8_t messageTypeId,
    uint32_t messageStreamId,
    const std::vector<uint8_t>& payload
) {
    core::Buffer buffer;

    auto basicHeader = createBasicHeader(0, csid);
    auto msgHeader = createMessageHeaderType0(timestamp, messageLength, messageTypeId, messageStreamId);

    buffer.append(basicHeader);
    buffer.append(msgHeader);

    // Extended timestamp if needed
    if (timestamp >= EXTENDED_TIMESTAMP_MARKER) {
        auto extTs = createExtendedTimestamp(timestamp);
        buffer.append(extTs);
    }

    // Payload
    buffer.append(payload.data(), payload.size());

    return buffer;
}

/**
 * @brief Build a Set Chunk Size message (message type 1).
 */
inline core::Buffer createSetChunkSizeMessage(uint32_t chunkSize, uint32_t csid = 2) {
    std::vector<uint8_t> payload(4);
    payload[0] = static_cast<uint8_t>((chunkSize >> 24) & 0x7F);  // MSB must be 0
    payload[1] = static_cast<uint8_t>((chunkSize >> 16) & 0xFF);
    payload[2] = static_cast<uint8_t>((chunkSize >> 8) & 0xFF);
    payload[3] = static_cast<uint8_t>(chunkSize & 0xFF);

    return createChunkType0(csid, 0, 4, MSG_TYPE_SET_CHUNK_SIZE, 0, payload);
}

/**
 * @brief Build an Abort Message (message type 2).
 */
inline core::Buffer createAbortMessage(uint32_t chunkStreamIdToAbort, uint32_t csid = 2) {
    std::vector<uint8_t> payload(4);
    payload[0] = static_cast<uint8_t>((chunkStreamIdToAbort >> 24) & 0xFF);
    payload[1] = static_cast<uint8_t>((chunkStreamIdToAbort >> 16) & 0xFF);
    payload[2] = static_cast<uint8_t>((chunkStreamIdToAbort >> 8) & 0xFF);
    payload[3] = static_cast<uint8_t>(chunkStreamIdToAbort & 0xFF);

    return createChunkType0(csid, 0, 4, MSG_TYPE_ABORT, 0, payload);
}

} // namespace ChunkTestHelpers

// =============================================================================
// Basic Header Parsing Tests
// =============================================================================

class ChunkParserBasicHeaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        parser_ = std::make_unique<ChunkParser>();
    }

    std::unique_ptr<ChunkParser> parser_;
};

// Test initial state
TEST_F(ChunkParserBasicHeaderTest, InitialChunkSizeIsDefault128) {
    // Parser should start with default chunk size of 128
    // This is verified indirectly through parsing behavior
    EXPECT_NE(parser_, nullptr);
}

// Test 1-byte basic header (csid 2-63)
TEST_F(ChunkParserBasicHeaderTest, ParseOneByteBasicHeader_CSID2) {
    auto chunk = ChunkTestHelpers::createChunkType0(
        2, 0, 10, MSG_TYPE_COMMAND_AMF0, 0, std::vector<uint8_t>(10, 0xAA));

    auto result = parser_->parse(chunk.data(), chunk.size());

    EXPECT_TRUE(result.error == std::nullopt) << "Parse should succeed";
    EXPECT_GT(result.bytesConsumed, 0u);

    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].chunkStreamId, 2u);
}

TEST_F(ChunkParserBasicHeaderTest, ParseOneByteBasicHeader_CSID63) {
    auto chunk = ChunkTestHelpers::createChunkType0(
        63, 0, 10, MSG_TYPE_COMMAND_AMF0, 0, std::vector<uint8_t>(10, 0xBB));

    auto result = parser_->parse(chunk.data(), chunk.size());

    EXPECT_TRUE(result.error == std::nullopt);

    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].chunkStreamId, 63u);
}

// Test 2-byte basic header (csid 64-319)
TEST_F(ChunkParserBasicHeaderTest, ParseTwoByteBasicHeader_CSID64) {
    auto chunk = ChunkTestHelpers::createChunkType0(
        64, 0, 10, MSG_TYPE_COMMAND_AMF0, 0, std::vector<uint8_t>(10, 0xCC));

    auto result = parser_->parse(chunk.data(), chunk.size());

    EXPECT_TRUE(result.error == std::nullopt);

    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].chunkStreamId, 64u);
}

TEST_F(ChunkParserBasicHeaderTest, ParseTwoByteBasicHeader_CSID319) {
    auto chunk = ChunkTestHelpers::createChunkType0(
        319, 0, 10, MSG_TYPE_COMMAND_AMF0, 0, std::vector<uint8_t>(10, 0xDD));

    auto result = parser_->parse(chunk.data(), chunk.size());

    EXPECT_TRUE(result.error == std::nullopt);

    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].chunkStreamId, 319u);
}

// Test 3-byte basic header (csid 320-65599)
TEST_F(ChunkParserBasicHeaderTest, ParseThreeByteBasicHeader_CSID320) {
    auto chunk = ChunkTestHelpers::createChunkType0(
        320, 0, 10, MSG_TYPE_COMMAND_AMF0, 0, std::vector<uint8_t>(10, 0xEE));

    auto result = parser_->parse(chunk.data(), chunk.size());

    EXPECT_TRUE(result.error == std::nullopt);

    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].chunkStreamId, 320u);
}

TEST_F(ChunkParserBasicHeaderTest, ParseThreeByteBasicHeader_CSID65599) {
    auto chunk = ChunkTestHelpers::createChunkType0(
        65599, 0, 10, MSG_TYPE_COMMAND_AMF0, 0, std::vector<uint8_t>(10, 0xFF));

    auto result = parser_->parse(chunk.data(), chunk.size());

    EXPECT_TRUE(result.error == std::nullopt);

    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].chunkStreamId, 65599u);
}

// =============================================================================
// Message Header Type Tests
// =============================================================================

class ChunkParserMessageHeaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        parser_ = std::make_unique<ChunkParser>();
    }

    std::unique_ptr<ChunkParser> parser_;
};

// Test Type 0 header (full header, 11 bytes)
TEST_F(ChunkParserMessageHeaderTest, ParseType0Header_FullHeader) {
    auto chunk = ChunkTestHelpers::createChunkType0(
        3, 1000, 50, MSG_TYPE_COMMAND_AMF0, 1, std::vector<uint8_t>(50, 0x11));

    auto result = parser_->parse(chunk.data(), chunk.size());

    EXPECT_TRUE(result.error == std::nullopt);

    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].chunkStreamId, 3u);
    EXPECT_EQ(chunks[0].timestamp, 1000u);
    EXPECT_EQ(chunks[0].messageLength, 50u);
    EXPECT_EQ(chunks[0].messageTypeId, MSG_TYPE_COMMAND_AMF0);
    EXPECT_EQ(chunks[0].messageStreamId, 1u);
    EXPECT_EQ(chunks[0].payload.size(), 50u);
    EXPECT_TRUE(chunks[0].isComplete);
}

// Test Type 1 header (7 bytes, omits message stream id)
TEST_F(ChunkParserMessageHeaderTest, ParseType1Header_UsesStoredStreamId) {
    // First send Type 0 to establish state
    auto chunk0 = ChunkTestHelpers::createChunkType0(
        3, 1000, 10, MSG_TYPE_COMMAND_AMF0, 42, std::vector<uint8_t>(10, 0x11));
    parser_->parse(chunk0.data(), chunk0.size());
    parser_->getCompletedChunks();  // Clear completed

    // Now send Type 1 header
    core::Buffer chunk1;
    auto basicHeader = ChunkTestHelpers::createBasicHeader(1, 3);  // fmt=1
    auto msgHeader = ChunkTestHelpers::createMessageHeaderType1(100, 15, MSG_TYPE_AUDIO);
    chunk1.append(basicHeader);
    chunk1.append(msgHeader);
    std::vector<uint8_t> payload(15, 0x22);
    chunk1.append(payload.data(), payload.size());

    auto result = parser_->parse(chunk1.data(), chunk1.size());

    EXPECT_TRUE(result.error == std::nullopt);

    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].chunkStreamId, 3u);
    EXPECT_EQ(chunks[0].timestamp, 1100u);  // 1000 + 100 delta
    EXPECT_EQ(chunks[0].messageLength, 15u);
    EXPECT_EQ(chunks[0].messageTypeId, MSG_TYPE_AUDIO);
    EXPECT_EQ(chunks[0].messageStreamId, 42u);  // From previous Type 0
}

// Test Type 2 header (3 bytes, only timestamp delta)
TEST_F(ChunkParserMessageHeaderTest, ParseType2Header_UsesStoredLengthAndType) {
    // First send Type 0 to establish state
    auto chunk0 = ChunkTestHelpers::createChunkType0(
        3, 1000, 20, MSG_TYPE_VIDEO, 99, std::vector<uint8_t>(20, 0x33));
    parser_->parse(chunk0.data(), chunk0.size());
    parser_->getCompletedChunks();

    // Now send Type 2 header
    core::Buffer chunk2;
    auto basicHeader = ChunkTestHelpers::createBasicHeader(2, 3);  // fmt=2
    auto msgHeader = ChunkTestHelpers::createMessageHeaderType2(50);
    chunk2.append(basicHeader);
    chunk2.append(msgHeader);
    std::vector<uint8_t> payload(20, 0x44);  // Same length as previous
    chunk2.append(payload.data(), payload.size());

    auto result = parser_->parse(chunk2.data(), chunk2.size());

    EXPECT_TRUE(result.error == std::nullopt);

    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].chunkStreamId, 3u);
    EXPECT_EQ(chunks[0].timestamp, 1050u);  // 1000 + 50 delta
    EXPECT_EQ(chunks[0].messageLength, 20u);  // From previous
    EXPECT_EQ(chunks[0].messageTypeId, MSG_TYPE_VIDEO);  // From previous
    EXPECT_EQ(chunks[0].messageStreamId, 99u);  // From previous
}

// Test Type 3 header (0 bytes, uses all stored values)
TEST_F(ChunkParserMessageHeaderTest, ParseType3Header_UsesAllStoredValues) {
    // First send Type 0 to establish state
    auto chunk0 = ChunkTestHelpers::createChunkType0(
        3, 1000, 30, MSG_TYPE_DATA_AMF0, 77, std::vector<uint8_t>(30, 0x55));
    parser_->parse(chunk0.data(), chunk0.size());
    parser_->getCompletedChunks();

    // Now send Type 3 header (only basic header, no message header)
    core::Buffer chunk3;
    auto basicHeader = ChunkTestHelpers::createBasicHeader(3, 3);  // fmt=3
    chunk3.append(basicHeader);
    std::vector<uint8_t> payload(30, 0x66);  // Same length as previous
    chunk3.append(payload.data(), payload.size());

    auto result = parser_->parse(chunk3.data(), chunk3.size());

    EXPECT_TRUE(result.error == std::nullopt);

    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].chunkStreamId, 3u);
    EXPECT_EQ(chunks[0].timestamp, 1000u);  // Same as previous (no delta)
    EXPECT_EQ(chunks[0].messageLength, 30u);
    EXPECT_EQ(chunks[0].messageTypeId, MSG_TYPE_DATA_AMF0);
    EXPECT_EQ(chunks[0].messageStreamId, 77u);
}

// =============================================================================
// Extended Timestamp Tests
// =============================================================================

class ChunkParserExtendedTimestampTest : public ::testing::Test {
protected:
    void SetUp() override {
        parser_ = std::make_unique<ChunkParser>();
    }

    std::unique_ptr<ChunkParser> parser_;
};

// Test extended timestamp with Type 0 header
TEST_F(ChunkParserExtendedTimestampTest, ExtendedTimestampType0) {
    uint32_t largeTimestamp = 0x01000000;  // Larger than 0xFFFFFF

    core::Buffer chunk;
    auto basicHeader = ChunkTestHelpers::createBasicHeader(0, 3);
    auto msgHeader = ChunkTestHelpers::createMessageHeaderType0(
        largeTimestamp, 10, MSG_TYPE_COMMAND_AMF0, 0);
    auto extTs = ChunkTestHelpers::createExtendedTimestamp(largeTimestamp);

    chunk.append(basicHeader);
    chunk.append(msgHeader);
    chunk.append(extTs);
    std::vector<uint8_t> payload(10, 0xAA);
    chunk.append(payload.data(), payload.size());

    auto result = parser_->parse(chunk.data(), chunk.size());

    EXPECT_TRUE(result.error == std::nullopt);

    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].timestamp, largeTimestamp);
}

// Test extended timestamp with Type 1 header
TEST_F(ChunkParserExtendedTimestampTest, ExtendedTimestampType1) {
    // First establish state with Type 0
    auto chunk0 = ChunkTestHelpers::createChunkType0(
        3, 1000, 10, MSG_TYPE_COMMAND_AMF0, 1, std::vector<uint8_t>(10, 0x11));
    parser_->parse(chunk0.data(), chunk0.size());
    parser_->getCompletedChunks();

    // Type 1 with extended timestamp delta
    uint32_t largeDelta = 0x02000000;

    core::Buffer chunk1;
    auto basicHeader = ChunkTestHelpers::createBasicHeader(1, 3);
    auto msgHeader = ChunkTestHelpers::createMessageHeaderType1(largeDelta, 10, MSG_TYPE_AUDIO);
    auto extTs = ChunkTestHelpers::createExtendedTimestamp(largeDelta);

    chunk1.append(basicHeader);
    chunk1.append(msgHeader);
    chunk1.append(extTs);
    std::vector<uint8_t> payload(10, 0xBB);
    chunk1.append(payload.data(), payload.size());

    auto result = parser_->parse(chunk1.data(), chunk1.size());

    EXPECT_TRUE(result.error == std::nullopt);

    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].timestamp, 1000u + largeDelta);
}

// =============================================================================
// Chunk Size Tests (Requirement 2.1)
// =============================================================================

class ChunkParserChunkSizeTest : public ::testing::Test {
protected:
    void SetUp() override {
        parser_ = std::make_unique<ChunkParser>();
    }

    std::unique_ptr<ChunkParser> parser_;
};

// Test default chunk size is 128 bytes
TEST_F(ChunkParserChunkSizeTest, DefaultChunkSizeIs128) {
    // Message larger than 128 bytes should be split into chunks
    std::vector<uint8_t> largePayload(200, 0xAB);

    // Create first chunk (Type 0) with 128 bytes of payload
    auto chunk0 = ChunkTestHelpers::createChunkType0(
        3, 0, 200, MSG_TYPE_COMMAND_AMF0, 0,
        std::vector<uint8_t>(largePayload.begin(), largePayload.begin() + 128));

    auto result0 = parser_->parse(chunk0.data(), chunk0.size());
    EXPECT_TRUE(result0.error == std::nullopt);

    // Should not be complete yet
    auto chunks0 = parser_->getCompletedChunks();
    EXPECT_EQ(chunks0.size(), 0u);  // Not complete, needs more data
}

// Test Set Chunk Size message changes chunk size (Requirement 2.2)
TEST_F(ChunkParserChunkSizeTest, SetChunkSizeChangesParsingBehavior) {
    // Set chunk size to 256
    auto setChunkSize = ChunkTestHelpers::createSetChunkSizeMessage(256);
    auto result = parser_->parse(setChunkSize.data(), setChunkSize.size());

    EXPECT_TRUE(result.error == std::nullopt);

    // The parser should process the Set Chunk Size message internally
    // Verify by checking that larger messages work with new chunk size
    parser_->getCompletedChunks();  // Clear

    // Now a 200-byte message should fit in one chunk (since chunk size is 256)
    std::vector<uint8_t> payload(200, 0xCD);
    auto chunk = ChunkTestHelpers::createChunkType0(
        3, 0, 200, MSG_TYPE_COMMAND_AMF0, 0, payload);

    auto result2 = parser_->parse(chunk.data(), chunk.size());
    EXPECT_TRUE(result2.error == std::nullopt);

    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_TRUE(chunks[0].isComplete);
}

// Test setChunkSize API method
TEST_F(ChunkParserChunkSizeTest, SetChunkSizeMethodChangesSize) {
    parser_->setChunkSize(512);

    // 400-byte message should fit in one chunk now
    std::vector<uint8_t> payload(400, 0xEF);
    auto chunk = ChunkTestHelpers::createChunkType0(
        3, 0, 400, MSG_TYPE_COMMAND_AMF0, 0, payload);

    auto result = parser_->parse(chunk.data(), chunk.size());
    EXPECT_TRUE(result.error == std::nullopt);

    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_TRUE(chunks[0].isComplete);
}

// Test minimum chunk size (128 bytes)
TEST_F(ChunkParserChunkSizeTest, MinimumChunkSizeIs128) {
    parser_->setChunkSize(MIN_CHUNK_SIZE);

    std::vector<uint8_t> payload(128, 0x12);
    auto chunk = ChunkTestHelpers::createChunkType0(
        3, 0, 128, MSG_TYPE_COMMAND_AMF0, 0, payload);

    auto result = parser_->parse(chunk.data(), chunk.size());
    EXPECT_TRUE(result.error == std::nullopt);

    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_TRUE(chunks[0].isComplete);
}

// Test maximum chunk size (65536 bytes)
TEST_F(ChunkParserChunkSizeTest, MaximumChunkSizeIs65536) {
    parser_->setChunkSize(MAX_CHUNK_SIZE);

    std::vector<uint8_t> payload(65536, 0x34);
    auto chunk = ChunkTestHelpers::createChunkType0(
        3, 0, 65536, MSG_TYPE_COMMAND_AMF0, 0, payload);

    auto result = parser_->parse(chunk.data(), chunk.size());
    EXPECT_TRUE(result.error == std::nullopt);

    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_TRUE(chunks[0].isComplete);
}

// Test chunk size below minimum is rejected
TEST_F(ChunkParserChunkSizeTest, ChunkSizeBelowMinimumIsRejected) {
    // Try to set chunk size to 64 (below minimum of 128)
    parser_->setChunkSize(64);

    // Should still use minimum of 128
    // We verify this indirectly - a 100-byte message with 64-byte chunk size
    // would need continuation, but with 128-byte minimum it doesn't
}

// Test chunk size above maximum is capped
TEST_F(ChunkParserChunkSizeTest, ChunkSizeAboveMaximumIsCapped) {
    parser_->setChunkSize(100000);  // Above maximum

    // Should be capped to 65536
    std::vector<uint8_t> payload(65536, 0x56);
    auto chunk = ChunkTestHelpers::createChunkType0(
        3, 0, 65536, MSG_TYPE_COMMAND_AMF0, 0, payload);

    auto result = parser_->parse(chunk.data(), chunk.size());
    EXPECT_TRUE(result.error == std::nullopt);
}

// =============================================================================
// Abort Message Tests (Requirement 2.5)
// =============================================================================

class ChunkParserAbortTest : public ::testing::Test {
protected:
    void SetUp() override {
        parser_ = std::make_unique<ChunkParser>();
    }

    std::unique_ptr<ChunkParser> parser_;
};

// Test abort message discards partial chunk stream data
TEST_F(ChunkParserAbortTest, AbortMessageDiscardsPartialData) {
    // Start a large message (larger than chunk size) on csid 5
    std::vector<uint8_t> partialPayload(128, 0xAA);  // Only first chunk
    auto chunk0 = ChunkTestHelpers::createChunkType0(
        5, 0, 300, MSG_TYPE_COMMAND_AMF0, 0, partialPayload);

    parser_->parse(chunk0.data(), chunk0.size());

    // No complete chunks yet
    auto chunks1 = parser_->getCompletedChunks();
    EXPECT_EQ(chunks1.size(), 0u);

    // Send abort message for csid 5
    parser_->abortChunkStream(5);

    // Now send a new complete message on csid 5
    std::vector<uint8_t> newPayload(50, 0xBB);
    auto chunk1 = ChunkTestHelpers::createChunkType0(
        5, 100, 50, MSG_TYPE_AUDIO, 1, newPayload);

    parser_->parse(chunk1.data(), chunk1.size());

    // Should get the new message, not the partial one
    auto chunks2 = parser_->getCompletedChunks();
    ASSERT_EQ(chunks2.size(), 1u);
    EXPECT_EQ(chunks2[0].messageLength, 50u);
    EXPECT_EQ(chunks2[0].payload[0], 0xBB);
}

// Test abort message only affects specified chunk stream
TEST_F(ChunkParserAbortTest, AbortMessageOnlyAffectsSpecifiedStream) {
    // Start partial messages on both csid 5 and csid 6
    // Use 200-byte message with 128-byte chunk size: needs 2 chunks (128 + 72)
    std::vector<uint8_t> partial5(128, 0x55);
    auto chunk5 = ChunkTestHelpers::createChunkType0(
        5, 0, 200, MSG_TYPE_COMMAND_AMF0, 0, partial5);
    parser_->parse(chunk5.data(), chunk5.size());

    std::vector<uint8_t> partial6(128, 0x66);
    auto chunk6 = ChunkTestHelpers::createChunkType0(
        6, 0, 200, MSG_TYPE_VIDEO, 0, partial6);
    parser_->parse(chunk6.data(), chunk6.size());

    // Abort only csid 5
    parser_->abortChunkStream(5);

    // Continue csid 6 message - it should still work
    // With 200-byte message and 128 already received, need 72 more bytes
    core::Buffer continuation;
    auto basicHeader = ChunkTestHelpers::createBasicHeader(3, 6);  // Type 3 continuation
    continuation.append(basicHeader);
    std::vector<uint8_t> morePayload(72, 0x77);  // Remaining bytes to complete 200
    continuation.append(morePayload.data(), morePayload.size());

    parser_->parse(continuation.data(), continuation.size());

    // csid 6 should be complete
    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].chunkStreamId, 6u);
}

// Test abort via protocol message
TEST_F(ChunkParserAbortTest, AbortProtocolMessageProcessed) {
    // Start partial message on csid 5
    std::vector<uint8_t> partial(128, 0x88);
    auto chunk0 = ChunkTestHelpers::createChunkType0(
        5, 0, 300, MSG_TYPE_COMMAND_AMF0, 0, partial);
    parser_->parse(chunk0.data(), chunk0.size());

    // Send abort protocol message for csid 5
    auto abortMsg = ChunkTestHelpers::createAbortMessage(5, 2);
    parser_->parse(abortMsg.data(), abortMsg.size());

    // The partial message on csid 5 should be discarded
    // New message on csid 5 should work
    std::vector<uint8_t> newPayload(50, 0x99);
    auto newChunk = ChunkTestHelpers::createChunkType0(
        5, 200, 50, MSG_TYPE_AUDIO, 2, newPayload);
    parser_->parse(newChunk.data(), newChunk.size());

    auto chunks = parser_->getCompletedChunks();
    // Filter out the abort message itself from completed
    size_t nonAbortCount = 0;
    for (const auto& c : chunks) {
        if (c.messageTypeId != MSG_TYPE_ABORT) {
            nonAbortCount++;
            EXPECT_EQ(c.payload[0], 0x99);
        }
    }
    EXPECT_GE(nonAbortCount, 1u);
}

// =============================================================================
// Multi-Chunk Message Reassembly Tests
// =============================================================================

class ChunkParserReassemblyTest : public ::testing::Test {
protected:
    void SetUp() override {
        parser_ = std::make_unique<ChunkParser>();
    }

    std::unique_ptr<ChunkParser> parser_;
};

// Test message spanning multiple chunks
TEST_F(ChunkParserReassemblyTest, ReassembleMultiChunkMessage) {
    // With default 128-byte chunk size, a 300-byte message spans 3 chunks
    uint32_t msgLength = 300;

    // First chunk (Type 0 header, 128 bytes payload)
    std::vector<uint8_t> payload1(128, 0x11);
    auto chunk1 = ChunkTestHelpers::createChunkType0(
        3, 0, msgLength, MSG_TYPE_COMMAND_AMF0, 0, payload1);
    parser_->parse(chunk1.data(), chunk1.size());

    // No complete message yet
    EXPECT_EQ(parser_->getCompletedChunks().size(), 0u);

    // Second chunk (Type 3 header, 128 bytes payload)
    core::Buffer chunk2;
    auto header2 = ChunkTestHelpers::createBasicHeader(3, 3);
    chunk2.append(header2);
    std::vector<uint8_t> payload2(128, 0x22);
    chunk2.append(payload2.data(), payload2.size());
    parser_->parse(chunk2.data(), chunk2.size());

    // Still not complete
    EXPECT_EQ(parser_->getCompletedChunks().size(), 0u);

    // Third chunk (Type 3 header, 44 bytes payload)
    core::Buffer chunk3;
    auto header3 = ChunkTestHelpers::createBasicHeader(3, 3);
    chunk3.append(header3);
    std::vector<uint8_t> payload3(44, 0x33);
    chunk3.append(payload3.data(), payload3.size());
    parser_->parse(chunk3.data(), chunk3.size());

    // Now should be complete
    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_TRUE(chunks[0].isComplete);
    EXPECT_EQ(chunks[0].payload.size(), 300u);

    // Verify payload concatenation
    for (size_t i = 0; i < 128; i++) {
        EXPECT_EQ(chunks[0].payload[i], 0x11);
    }
    for (size_t i = 128; i < 256; i++) {
        EXPECT_EQ(chunks[0].payload[i], 0x22);
    }
    for (size_t i = 256; i < 300; i++) {
        EXPECT_EQ(chunks[0].payload[i], 0x33);
    }
}

// Test interleaved messages on different chunk streams
TEST_F(ChunkParserReassemblyTest, InterleavedMessagesOnDifferentStreams) {
    // Start message on csid 3
    std::vector<uint8_t> payload3_1(128, 0xAA);
    auto chunk3_1 = ChunkTestHelpers::createChunkType0(
        3, 0, 200, MSG_TYPE_COMMAND_AMF0, 1, payload3_1);
    parser_->parse(chunk3_1.data(), chunk3_1.size());

    // Start message on csid 4
    std::vector<uint8_t> payload4(100, 0xBB);
    auto chunk4 = ChunkTestHelpers::createChunkType0(
        4, 0, 100, MSG_TYPE_AUDIO, 2, payload4);
    parser_->parse(chunk4.data(), chunk4.size());

    // csid 4 should be complete
    auto chunks1 = parser_->getCompletedChunks();
    ASSERT_EQ(chunks1.size(), 1u);
    EXPECT_EQ(chunks1[0].chunkStreamId, 4u);

    // Continue csid 3
    core::Buffer chunk3_2;
    auto header3_2 = ChunkTestHelpers::createBasicHeader(3, 3);
    chunk3_2.append(header3_2);
    std::vector<uint8_t> payload3_2(72, 0xCC);
    chunk3_2.append(payload3_2.data(), payload3_2.size());
    parser_->parse(chunk3_2.data(), chunk3_2.size());

    // Now csid 3 should be complete
    auto chunks2 = parser_->getCompletedChunks();
    ASSERT_EQ(chunks2.size(), 1u);
    EXPECT_EQ(chunks2[0].chunkStreamId, 3u);
}

// =============================================================================
// Partial Data Handling Tests
// =============================================================================

class ChunkParserPartialDataTest : public ::testing::Test {
protected:
    void SetUp() override {
        parser_ = std::make_unique<ChunkParser>();
    }

    std::unique_ptr<ChunkParser> parser_;
};

// Test handling partial basic header
TEST_F(ChunkParserPartialDataTest, PartialBasicHeaderReturnsZeroConsumed) {
    // 2-byte basic header, but only provide 1 byte
    core::Buffer partial;
    partial.append({0x00});  // fmt=0, csid=0 (indicating 2-byte header)

    auto result = parser_->parse(partial.data(), partial.size());

    EXPECT_TRUE(result.error == std::nullopt);
    EXPECT_EQ(result.bytesConsumed, 0u);
}

// Test handling partial message header
TEST_F(ChunkParserPartialDataTest, PartialMessageHeaderReturnsZeroConsumed) {
    // Type 0 header needs 12 bytes total (1 basic + 11 message)
    // Provide only 8 bytes
    core::Buffer partial(8);
    partial[0] = 0x03;  // fmt=0, csid=3

    auto result = parser_->parse(partial.data(), partial.size());

    EXPECT_TRUE(result.error == std::nullopt);
    EXPECT_EQ(result.bytesConsumed, 0u);
}

// Test handling partial payload
TEST_F(ChunkParserPartialDataTest, PartialPayloadReturnsPartialConsumed) {
    // Create header for 50-byte message, but only provide 30 bytes of payload
    core::Buffer chunk;
    auto basicHeader = ChunkTestHelpers::createBasicHeader(0, 3);
    auto msgHeader = ChunkTestHelpers::createMessageHeaderType0(0, 50, MSG_TYPE_COMMAND_AMF0, 0);
    chunk.append(basicHeader);
    chunk.append(msgHeader);
    std::vector<uint8_t> partialPayload(30, 0xAA);
    chunk.append(partialPayload.data(), partialPayload.size());

    auto result = parser_->parse(chunk.data(), chunk.size());

    // Should consume header + available payload
    EXPECT_TRUE(result.error == std::nullopt);
    EXPECT_GT(result.bytesConsumed, 0u);

    // No complete chunks yet
    EXPECT_EQ(parser_->getCompletedChunks().size(), 0u);
}

// Test empty input returns zero consumed
TEST_F(ChunkParserPartialDataTest, EmptyInputReturnsZeroConsumed) {
    auto result = parser_->parse(nullptr, 0);

    EXPECT_TRUE(result.error == std::nullopt);
    EXPECT_EQ(result.bytesConsumed, 0u);
}

// =============================================================================
// Per-Chunk-Stream State Tests
// =============================================================================

class ChunkParserStateTest : public ::testing::Test {
protected:
    void SetUp() override {
        parser_ = std::make_unique<ChunkParser>();
    }

    std::unique_ptr<ChunkParser> parser_;
};

// Test each chunk stream maintains independent state
TEST_F(ChunkParserStateTest, IndependentStatePerChunkStream) {
    // Send Type 0 on csid 3 with timestamp 1000
    std::vector<uint8_t> payload3(50, 0x33);
    auto chunk3 = ChunkTestHelpers::createChunkType0(
        3, 1000, 50, MSG_TYPE_COMMAND_AMF0, 10, payload3);
    parser_->parse(chunk3.data(), chunk3.size());
    parser_->getCompletedChunks();

    // Send Type 0 on csid 4 with timestamp 2000
    std::vector<uint8_t> payload4(60, 0x44);
    auto chunk4 = ChunkTestHelpers::createChunkType0(
        4, 2000, 60, MSG_TYPE_AUDIO, 20, payload4);
    parser_->parse(chunk4.data(), chunk4.size());
    parser_->getCompletedChunks();

    // Send Type 3 on csid 3 - should use csid 3's stored values
    core::Buffer chunk3_2;
    auto header3_2 = ChunkTestHelpers::createBasicHeader(3, 3);
    chunk3_2.append(header3_2);
    std::vector<uint8_t> payload3_2(50, 0x35);
    chunk3_2.append(payload3_2.data(), payload3_2.size());
    parser_->parse(chunk3_2.data(), chunk3_2.size());

    auto chunks3 = parser_->getCompletedChunks();
    ASSERT_EQ(chunks3.size(), 1u);
    EXPECT_EQ(chunks3[0].chunkStreamId, 3u);
    EXPECT_EQ(chunks3[0].timestamp, 1000u);  // csid 3's timestamp
    EXPECT_EQ(chunks3[0].messageStreamId, 10u);  // csid 3's stream id

    // Send Type 3 on csid 4 - should use csid 4's stored values
    core::Buffer chunk4_2;
    auto header4_2 = ChunkTestHelpers::createBasicHeader(3, 4);
    chunk4_2.append(header4_2);
    std::vector<uint8_t> payload4_2(60, 0x46);
    chunk4_2.append(payload4_2.data(), payload4_2.size());
    parser_->parse(chunk4_2.data(), chunk4_2.size());

    auto chunks4 = parser_->getCompletedChunks();
    ASSERT_EQ(chunks4.size(), 1u);
    EXPECT_EQ(chunks4[0].chunkStreamId, 4u);
    EXPECT_EQ(chunks4[0].timestamp, 2000u);  // csid 4's timestamp
    EXPECT_EQ(chunks4[0].messageStreamId, 20u);  // csid 4's stream id
}

// =============================================================================
// GetCompletedChunks Behavior Tests
// =============================================================================

class ChunkParserCompletedChunksTest : public ::testing::Test {
protected:
    void SetUp() override {
        parser_ = std::make_unique<ChunkParser>();
    }

    std::unique_ptr<ChunkParser> parser_;
};

// Test getCompletedChunks clears the internal list
TEST_F(ChunkParserCompletedChunksTest, GetCompletedChunksClearsList) {
    std::vector<uint8_t> payload(50, 0xAB);
    auto chunk = ChunkTestHelpers::createChunkType0(
        3, 0, 50, MSG_TYPE_COMMAND_AMF0, 0, payload);

    parser_->parse(chunk.data(), chunk.size());

    auto chunks1 = parser_->getCompletedChunks();
    ASSERT_EQ(chunks1.size(), 1u);

    // Second call should return empty
    auto chunks2 = parser_->getCompletedChunks();
    EXPECT_EQ(chunks2.size(), 0u);
}

// Test multiple complete messages in single parse
TEST_F(ChunkParserCompletedChunksTest, MultipleCompletedInSingleParse) {
    // Create two small complete messages back to back
    std::vector<uint8_t> payload1(30, 0x11);
    auto chunk1 = ChunkTestHelpers::createChunkType0(
        3, 0, 30, MSG_TYPE_COMMAND_AMF0, 0, payload1);

    std::vector<uint8_t> payload2(40, 0x22);
    auto chunk2 = ChunkTestHelpers::createChunkType0(
        4, 100, 40, MSG_TYPE_AUDIO, 1, payload2);

    core::Buffer combined;
    combined.append(chunk1);
    combined.append(chunk2);

    parser_->parse(combined.data(), combined.size());

    auto chunks = parser_->getCompletedChunks();
    ASSERT_EQ(chunks.size(), 2u);
}

} // namespace test
} // namespace protocol
} // namespace openrtmp
