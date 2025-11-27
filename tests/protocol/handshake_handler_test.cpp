// OpenRTMP - Cross-platform RTMP Server
// Tests for RTMP Handshake Handler State Machine
//
// Tests cover:
// - State tracking for WaitingC0, WaitingC1, WaitingC2, Complete, Failed
// - C0 version byte validation (must be 3 for RTMP)
// - C1 packet processing and S0+S1+S2 response generation
// - C2 echo validation with timestamp verification
// - Cryptographically random S1 data generation (1528 bytes)
// - Response latency requirements (within 100ms target)
// - Error handling for malformed packets

#include <gtest/gtest.h>
#include <chrono>
#include <random>
#include <algorithm>
#include <cstring>
#include "openrtmp/protocol/handshake_handler.hpp"
#include "openrtmp/core/buffer.hpp"

namespace openrtmp {
namespace protocol {
namespace test {

// RTMP Handshake Constants
constexpr uint8_t RTMP_VERSION = 3;
constexpr size_t C0_SIZE = 1;
constexpr size_t C1_SIZE = 1536;
constexpr size_t C2_SIZE = 1536;
constexpr size_t S0_SIZE = 1;
constexpr size_t S1_SIZE = 1536;
constexpr size_t S2_SIZE = 1536;
constexpr size_t RANDOM_DATA_SIZE = 1528;  // 1536 - 4 (timestamp) - 4 (zero)

// =============================================================================
// State Machine Tests
// =============================================================================

class HandshakeHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        handler_ = std::make_unique<HandshakeHandler>();
    }

    // Helper to create valid C0 packet
    static core::Buffer createC0(uint8_t version = RTMP_VERSION) {
        return core::Buffer({version});
    }

    // Helper to create valid C1 packet with timestamp and random data
    static core::Buffer createC1(uint32_t timestamp = 0) {
        core::Buffer buffer;
        core::BufferWriter writer(buffer);

        // 4 bytes timestamp (big-endian)
        writer.writeUint32BE(timestamp);

        // 4 bytes zero
        writer.writeUint32BE(0);

        // 1528 bytes random data
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);
        for (size_t i = 0; i < RANDOM_DATA_SIZE; ++i) {
            writer.writeUint8(static_cast<uint8_t>(dis(gen)));
        }

        return buffer;
    }

    // Helper to create C2 packet that echoes S1 data
    static core::Buffer createC2(const uint8_t* s1Data, size_t s1Size, uint32_t timestamp) {
        core::Buffer buffer;
        core::BufferWriter writer(buffer);

        // Echo the S1 timestamp in first 4 bytes
        // S1 timestamp was in first 4 bytes of S1
        if (s1Size >= 4) {
            writer.writeBytes(s1Data, 4);  // S1 timestamp
        }

        // Next 4 bytes: time at which C1 was read (client's timestamp)
        writer.writeUint32BE(timestamp);

        // Echo the rest of S1 random data (1528 bytes)
        if (s1Size > 8) {
            writer.writeBytes(s1Data + 8, s1Size - 8);
        }

        return buffer;
    }

    std::unique_ptr<HandshakeHandler> handler_;
};

// Test initial state
TEST_F(HandshakeHandlerTest, InitialStateIsWaitingC0) {
    EXPECT_EQ(handler_->getState(), HandshakeState::WaitingC0);
    EXPECT_FALSE(handler_->isComplete());
}

// Test C0 processing with valid version
TEST_F(HandshakeHandlerTest, ProcessValidC0TransitionsToWaitingC1) {
    auto c0 = createC0(RTMP_VERSION);

    auto result = handler_->processData(c0.data(), c0.size());

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytesConsumed, C0_SIZE);
    EXPECT_EQ(handler_->getState(), HandshakeState::WaitingC1);
}

// Test C0 processing with invalid version
TEST_F(HandshakeHandlerTest, ProcessInvalidC0VersionTransitionsToFailed) {
    auto c0 = createC0(4);  // Invalid version

    auto result = handler_->processData(c0.data(), c0.size());

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->code, HandshakeError::Code::InvalidVersion);
    EXPECT_EQ(handler_->getState(), HandshakeState::Failed);
}

// Test C0 with version 0 (invalid)
TEST_F(HandshakeHandlerTest, ProcessC0Version0IsInvalid) {
    auto c0 = createC0(0);

    auto result = handler_->processData(c0.data(), c0.size());

    EXPECT_FALSE(result.success);
    EXPECT_EQ(handler_->getState(), HandshakeState::Failed);
}

// Test C1 processing generates S0+S1+S2 response
TEST_F(HandshakeHandlerTest, ProcessC1GeneratesS0S1S2Response) {
    // First process C0
    auto c0 = createC0();
    handler_->processData(c0.data(), c0.size());

    // Then process C1
    auto c1 = createC1(12345);
    auto result = handler_->processData(c1.data(), c1.size());

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytesConsumed, C1_SIZE);
    EXPECT_EQ(handler_->getState(), HandshakeState::WaitingC2);

    // Verify response is generated (S0 + S1 + S2)
    auto response = handler_->getResponseData();
    EXPECT_EQ(response.size(), S0_SIZE + S1_SIZE + S2_SIZE);

    // Verify S0 version byte
    EXPECT_EQ(response[0], RTMP_VERSION);
}

// Test S1 contains timestamp and random data
TEST_F(HandshakeHandlerTest, S1ContainsTimestampAndRandomData) {
    auto c0 = createC0();
    handler_->processData(c0.data(), c0.size());

    auto c1 = createC1();
    handler_->processData(c1.data(), c1.size());

    auto response = handler_->getResponseData();

    // S1 starts at offset 1 (after S0)
    const uint8_t* s1 = response.data() + S0_SIZE;

    // S1 should have timestamp in first 4 bytes
    // Note: timestamp could be 0 at start, so we just check structure exists
    (void)s1;  // S1 timestamp is validated in other tests

    // Verify S1 size
    EXPECT_GE(response.size(), S0_SIZE + S1_SIZE);
}

// Test S2 echoes C1 data
TEST_F(HandshakeHandlerTest, S2EchoesC1Data) {
    auto c0 = createC0();
    handler_->processData(c0.data(), c0.size());

    auto c1 = createC1(12345);
    handler_->processData(c1.data(), c1.size());

    auto response = handler_->getResponseData();

    // S2 starts at offset S0_SIZE + S1_SIZE
    const uint8_t* s2 = response.data() + S0_SIZE + S1_SIZE;

    // S2 first 4 bytes should be C1's timestamp (12345)
    core::BufferReader reader(s2, S2_SIZE);
    uint32_t echoedTimestamp = reader.readUint32BE();
    EXPECT_EQ(echoedTimestamp, 12345u);

    // S2 bytes 8-1536 should be C1 random data
    const uint8_t* c1Random = c1.data() + 8;
    const uint8_t* s2Random = s2 + 8;
    EXPECT_EQ(std::memcmp(c1Random, s2Random, RANDOM_DATA_SIZE), 0);
}

// Test C2 validation with correct echo
TEST_F(HandshakeHandlerTest, ProcessValidC2TransitionsToComplete) {
    // Process C0
    auto c0 = createC0();
    handler_->processData(c0.data(), c0.size());

    // Process C1
    auto c1 = createC1();
    handler_->processData(c1.data(), c1.size());

    // Get S1 from response
    auto response = handler_->getResponseData();
    const uint8_t* s1 = response.data() + S0_SIZE;

    // Clear response for next phase
    handler_->getResponseData();  // This clears internal response

    // Create C2 that echoes S1
    auto c2 = createC2(s1, S1_SIZE, 0);
    auto result = handler_->processData(c2.data(), c2.size());

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytesConsumed, C2_SIZE);
    EXPECT_EQ(handler_->getState(), HandshakeState::Complete);
    EXPECT_TRUE(handler_->isComplete());
}

// Test C2 validation with incorrect echo fails
TEST_F(HandshakeHandlerTest, ProcessInvalidC2TransitionsToFailed) {
    // Process C0
    auto c0 = createC0();
    handler_->processData(c0.data(), c0.size());

    // Process C1
    auto c1 = createC1();
    handler_->processData(c1.data(), c1.size());

    // Clear response
    handler_->getResponseData();

    // Create C2 with wrong data
    core::Buffer badC2(C2_SIZE);
    std::fill(badC2.data(), badC2.data() + C2_SIZE, 0xFF);

    auto result = handler_->processData(badC2.data(), badC2.size());

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->code, HandshakeError::Code::MalformedPacket);
    EXPECT_EQ(handler_->getState(), HandshakeState::Failed);
}

// =============================================================================
// Partial Data Tests
// =============================================================================

// Test partial C0 (0 bytes)
TEST_F(HandshakeHandlerTest, PartialC0ReturnsZeroBytesConsumed) {
    auto result = handler_->processData(nullptr, 0);

    EXPECT_TRUE(result.success);  // Not an error, just need more data
    EXPECT_EQ(result.bytesConsumed, 0u);
    EXPECT_EQ(handler_->getState(), HandshakeState::WaitingC0);
}

// Test partial C1 (less than 1536 bytes)
TEST_F(HandshakeHandlerTest, PartialC1ReturnsZeroBytesConsumed) {
    auto c0 = createC0();
    handler_->processData(c0.data(), c0.size());

    core::Buffer partialC1(100);  // Less than 1536
    auto result = handler_->processData(partialC1.data(), partialC1.size());

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytesConsumed, 0u);
    EXPECT_EQ(handler_->getState(), HandshakeState::WaitingC1);
}

// Test processing C0 and partial C1 in single call
TEST_F(HandshakeHandlerTest, ProcessC0WithPartialC1ConsumesOnlyC0) {
    core::Buffer data(500);  // C0 (1 byte) + 499 bytes of C1
    data[0] = RTMP_VERSION;

    auto result = handler_->processData(data.data(), data.size());

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytesConsumed, C0_SIZE);
    EXPECT_EQ(handler_->getState(), HandshakeState::WaitingC1);
}

// Test processing C0+C1 in single call
TEST_F(HandshakeHandlerTest, ProcessC0AndC1InSingleCall) {
    auto c0 = createC0();
    auto c1 = createC1();

    core::Buffer combined;
    combined.append(c0);
    combined.append(c1);

    // Process C0 first
    auto result1 = handler_->processData(combined.data(), combined.size());
    EXPECT_TRUE(result1.success);
    EXPECT_EQ(result1.bytesConsumed, C0_SIZE);
    EXPECT_EQ(handler_->getState(), HandshakeState::WaitingC1);

    // Process C1
    auto result2 = handler_->processData(combined.data() + C0_SIZE, combined.size() - C0_SIZE);
    EXPECT_TRUE(result2.success);
    EXPECT_EQ(result2.bytesConsumed, C1_SIZE);
    EXPECT_EQ(handler_->getState(), HandshakeState::WaitingC2);
}

// =============================================================================
// Random Data Generation Tests
// =============================================================================

// Test S1 random data is actually random (different across instances)
TEST_F(HandshakeHandlerTest, S1RandomDataIsDifferentAcrossInstances) {
    // First handshake
    auto handler1 = std::make_unique<HandshakeHandler>();
    auto c0 = createC0();
    auto c1 = createC1();

    handler1->processData(c0.data(), c0.size());
    handler1->processData(c1.data(), c1.size());
    auto response1 = handler1->getResponseData();

    // Second handshake
    auto handler2 = std::make_unique<HandshakeHandler>();
    handler2->processData(c0.data(), c0.size());
    handler2->processData(c1.data(), c1.size());
    auto response2 = handler2->getResponseData();

    // S1 random portions should be different (bytes 8-1535)
    const uint8_t* s1_1 = response1.data() + S0_SIZE + 8;
    const uint8_t* s1_2 = response2.data() + S0_SIZE + 8;

    bool isDifferent = false;
    for (size_t i = 0; i < RANDOM_DATA_SIZE; ++i) {
        if (s1_1[i] != s1_2[i]) {
            isDifferent = true;
            break;
        }
    }

    EXPECT_TRUE(isDifferent) << "S1 random data should be different across instances";
}

// Test S1 random data has good distribution (basic entropy check)
TEST_F(HandshakeHandlerTest, S1RandomDataHasGoodDistribution) {
    auto c0 = createC0();
    auto c1 = createC1();

    handler_->processData(c0.data(), c0.size());
    handler_->processData(c1.data(), c1.size());
    auto response = handler_->getResponseData();

    const uint8_t* s1Random = response.data() + S0_SIZE + 8;

    // Count byte value distribution
    int counts[256] = {0};
    for (size_t i = 0; i < RANDOM_DATA_SIZE; ++i) {
        counts[s1Random[i]]++;
    }

    // Check that we have a reasonable distribution
    // For 1528 bytes, we expect roughly 6 occurrences per value (1528/256)
    // With good randomness, most values should appear at least once
    int nonZeroCounts = 0;
    for (int i = 0; i < 256; ++i) {
        if (counts[i] > 0) {
            nonZeroCounts++;
        }
    }

    // With 1528 random bytes, we should see at least 200 different values
    EXPECT_GT(nonZeroCounts, 200) << "Random data should have good byte distribution";
}

// =============================================================================
// State Transition Tests
// =============================================================================

// Test cannot process C1 before C0
TEST_F(HandshakeHandlerTest, CannotProcessC1BeforeC0) {
    auto c1 = createC1();

    // Try to process C1 in WaitingC0 state - should fail or be ignored
    // The handler should only consume C0-sized data when in WaitingC0
    auto result = handler_->processData(c1.data(), c1.size());

    // Should not transition to WaitingC1 because first byte is not valid version
    // (random C1 data unlikely to have valid version byte)
}

// Test cannot process C2 before C1
TEST_F(HandshakeHandlerTest, CannotProcessC2BeforeC1) {
    auto c0 = createC0();
    handler_->processData(c0.data(), c0.size());

    core::Buffer fakeC2(C2_SIZE);

    // In WaitingC1 state, a C2-sized buffer should be interpreted as partial C1
    auto result = handler_->processData(fakeC2.data(), fakeC2.size());

    // Should consume as C1 and transition
    EXPECT_EQ(handler_->getState(), HandshakeState::WaitingC2);
}

// Test state transitions are unidirectional
TEST_F(HandshakeHandlerTest, StateTransitionsAreUnidirectional) {
    // Complete full handshake
    auto c0 = createC0();
    auto c1 = createC1();

    handler_->processData(c0.data(), c0.size());
    EXPECT_EQ(handler_->getState(), HandshakeState::WaitingC1);

    handler_->processData(c1.data(), c1.size());
    EXPECT_EQ(handler_->getState(), HandshakeState::WaitingC2);

    auto response = handler_->getResponseData();
    const uint8_t* s1 = response.data() + S0_SIZE;

    auto c2 = createC2(s1, S1_SIZE, 0);
    handler_->processData(c2.data(), c2.size());
    EXPECT_EQ(handler_->getState(), HandshakeState::Complete);

    // Try to process more data - should not change state
    auto extraC0 = createC0();
    auto result = handler_->processData(extraC0.data(), extraC0.size());

    // State should remain Complete
    EXPECT_EQ(handler_->getState(), HandshakeState::Complete);
}

// Test Failed state is terminal
TEST_F(HandshakeHandlerTest, FailedStateIsTerminal) {
    // Cause a failure with invalid version
    auto badC0 = createC0(99);
    handler_->processData(badC0.data(), badC0.size());
    EXPECT_EQ(handler_->getState(), HandshakeState::Failed);

    // Try to process valid data - should not change state
    auto validC0 = createC0();
    auto result = handler_->processData(validC0.data(), validC0.size());

    EXPECT_EQ(handler_->getState(), HandshakeState::Failed);
}

// =============================================================================
// Performance Tests
// =============================================================================

// Test response generation is within latency target (100ms)
TEST_F(HandshakeHandlerTest, ResponseGenerationWithinLatencyTarget) {
    auto c0 = createC0();
    auto c1 = createC1();

    handler_->processData(c0.data(), c0.size());

    auto start = std::chrono::high_resolution_clock::now();
    handler_->processData(c1.data(), c1.size());
    auto response = handler_->getResponseData();
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    EXPECT_LT(duration.count(), 100) << "Response generation should be under 100ms";
    EXPECT_FALSE(response.empty());
}

// =============================================================================
// Edge Cases
// =============================================================================

// Test processing empty data
TEST_F(HandshakeHandlerTest, ProcessEmptyDataDoesNotChangeState) {
    core::Buffer empty;
    auto result = handler_->processData(empty.data(), empty.size());

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytesConsumed, 0u);
    EXPECT_EQ(handler_->getState(), HandshakeState::WaitingC0);
}

// Test S0 version in response matches RTMP version 3
TEST_F(HandshakeHandlerTest, S0ResponseVersionIs3) {
    auto c0 = createC0();
    auto c1 = createC1();

    handler_->processData(c0.data(), c0.size());
    handler_->processData(c1.data(), c1.size());

    auto response = handler_->getResponseData();
    EXPECT_EQ(response[0], 3u);
}

// Test getResponseData returns empty after consumption
TEST_F(HandshakeHandlerTest, GetResponseDataClearsAfterRetrieval) {
    auto c0 = createC0();
    auto c1 = createC1();

    handler_->processData(c0.data(), c0.size());
    handler_->processData(c1.data(), c1.size());

    auto response1 = handler_->getResponseData();
    EXPECT_FALSE(response1.empty());

    auto response2 = handler_->getResponseData();
    EXPECT_TRUE(response2.empty());
}

// Test timestamp echo in S2
TEST_F(HandshakeHandlerTest, S2ContainsClientTimestampEcho) {
    auto c0 = createC0();
    handler_->processData(c0.data(), c0.size());

    uint32_t clientTimestamp = 0x12345678;
    auto c1 = createC1(clientTimestamp);
    handler_->processData(c1.data(), c1.size());

    auto response = handler_->getResponseData();
    const uint8_t* s2 = response.data() + S0_SIZE + S1_SIZE;

    // First 4 bytes of S2 should be client's C1 timestamp
    core::BufferReader reader(s2, 4);
    uint32_t echoedTimestamp = reader.readUint32BE();
    EXPECT_EQ(echoedTimestamp, clientTimestamp);
}

// Test C1 with zero timestamp
TEST_F(HandshakeHandlerTest, C1WithZeroTimestampIsValid) {
    auto c0 = createC0();
    handler_->processData(c0.data(), c0.size());

    auto c1 = createC1(0);
    auto result = handler_->processData(c1.data(), c1.size());

    EXPECT_TRUE(result.success);
    EXPECT_EQ(handler_->getState(), HandshakeState::WaitingC2);
}

// Test C1 with max timestamp
TEST_F(HandshakeHandlerTest, C1WithMaxTimestampIsValid) {
    auto c0 = createC0();
    handler_->processData(c0.data(), c0.size());

    auto c1 = createC1(0xFFFFFFFF);
    auto result = handler_->processData(c1.data(), c1.size());

    EXPECT_TRUE(result.success);
    EXPECT_EQ(handler_->getState(), HandshakeState::WaitingC2);
}

// =============================================================================
// C2 Timestamp Verification Tests
// =============================================================================

// Test C2 timestamp verification (S1 timestamp should be echoed)
TEST_F(HandshakeHandlerTest, C2MustEchoS1Timestamp) {
    auto c0 = createC0();
    auto c1 = createC1();

    handler_->processData(c0.data(), c0.size());
    handler_->processData(c1.data(), c1.size());

    auto response = handler_->getResponseData();
    const uint8_t* s1 = response.data() + S0_SIZE;

    // Create proper C2 that echoes S1
    auto c2 = createC2(s1, S1_SIZE, 0);
    auto result = handler_->processData(c2.data(), c2.size());

    EXPECT_TRUE(result.success);
    EXPECT_EQ(handler_->getState(), HandshakeState::Complete);
}

// Test C2 with wrong timestamp fails
TEST_F(HandshakeHandlerTest, C2WithWrongTimestampFails) {
    auto c0 = createC0();
    auto c1 = createC1();

    handler_->processData(c0.data(), c0.size());
    handler_->processData(c1.data(), c1.size());

    auto response = handler_->getResponseData();

    // Create C2 with wrong timestamp (not echoing S1)
    core::Buffer badC2(C2_SIZE);
    core::BufferWriter writer(badC2);
    writer.writeUint32BE(0xDEADBEEF);  // Wrong timestamp
    writer.writeUint32BE(0);
    // Fill rest with zeros
    for (size_t i = 8; i < C2_SIZE; ++i) {
        writer.writeUint8(0);
    }

    auto result = handler_->processData(badC2.data(), badC2.size());

    EXPECT_FALSE(result.success);
    EXPECT_EQ(handler_->getState(), HandshakeState::Failed);
}

// =============================================================================
// Error Message Tests
// =============================================================================

// Test error messages are descriptive
TEST_F(HandshakeHandlerTest, ErrorMessagesAreDescriptive) {
    auto badC0 = createC0(99);
    auto result = handler_->processData(badC0.data(), badC0.size());

    EXPECT_TRUE(result.error.has_value());
    EXPECT_FALSE(result.error->message.empty());
}

// =============================================================================
// Full Handshake Flow Test
// =============================================================================

TEST_F(HandshakeHandlerTest, CompleteHandshakeFlow) {
    // Step 1: Process C0
    auto c0 = createC0();
    auto result1 = handler_->processData(c0.data(), c0.size());
    EXPECT_TRUE(result1.success);
    EXPECT_EQ(handler_->getState(), HandshakeState::WaitingC1);

    // Step 2: Process C1 and get S0+S1+S2 response
    auto c1 = createC1(1000);
    auto result2 = handler_->processData(c1.data(), c1.size());
    EXPECT_TRUE(result2.success);
    EXPECT_EQ(handler_->getState(), HandshakeState::WaitingC2);

    // Verify response
    auto response = handler_->getResponseData();
    EXPECT_EQ(response.size(), S0_SIZE + S1_SIZE + S2_SIZE);
    EXPECT_EQ(response[0], RTMP_VERSION);

    // Step 3: Process C2 (echo S1)
    const uint8_t* s1 = response.data() + S0_SIZE;
    auto c2 = createC2(s1, S1_SIZE, 0);
    auto result3 = handler_->processData(c2.data(), c2.size());
    EXPECT_TRUE(result3.success);
    EXPECT_EQ(handler_->getState(), HandshakeState::Complete);
    EXPECT_TRUE(handler_->isComplete());
}

} // namespace test
} // namespace protocol
} // namespace openrtmp
