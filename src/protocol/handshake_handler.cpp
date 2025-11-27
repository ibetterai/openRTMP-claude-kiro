// OpenRTMP - Cross-platform RTMP Server
// RTMP Handshake Handler Implementation
//
// Implements the RTMP handshake state machine with:
// - State tracking: WaitingC0 -> WaitingC1 -> WaitingC2 -> Complete/Failed
// - Cryptographically random S1 data generation (1528 bytes)
// - C0 version validation (must be 3)
// - C1/S2 processing and response generation
// - C2 echo validation with timestamp verification
// - Response latency target: <100ms

#include "openrtmp/protocol/handshake_handler.hpp"

#include <chrono>
#include <random>
#include <cstring>
#include <algorithm>

// Platform-specific includes for cryptographic random
#if defined(_WIN32) || defined(_WIN64)
    #include <windows.h>
    #include <bcrypt.h>
#elif defined(__APPLE__)
    #include <Security/Security.h>
#elif defined(__linux__) || defined(__ANDROID__)
    #include <sys/random.h>
#endif

namespace openrtmp {
namespace protocol {

HandshakeHandler::HandshakeHandler()
    : state_(HandshakeState::WaitingC0)
    , serverTimestamp_(getCurrentTimestamp())
{
    // Pre-allocate S1 data buffer
    s1Data_.resize(handshake::S1_SIZE);

    // Generate S1: timestamp (4 bytes) + zero (4 bytes) + random (1528 bytes)
    core::Buffer s1Buffer;
    core::BufferWriter writer(s1Buffer);

    // Write server timestamp
    writer.writeUint32BE(serverTimestamp_);

    // Write zero bytes
    writer.writeUint32BE(0);

    // Generate 1528 bytes of cryptographically random data
    std::vector<uint8_t> randomData(handshake::RANDOM_SIZE);
    generateRandomBytes(randomData.data(), randomData.size());
    writer.writeBytes(randomData);

    // Store S1 data for later use
    std::copy(s1Buffer.begin(), s1Buffer.end(), s1Data_.begin());
}

HandshakeResult HandshakeHandler::processData(const uint8_t* data, size_t length) {
    // Handle terminal states
    if (state_ == HandshakeState::Complete || state_ == HandshakeState::Failed) {
        return HandshakeResult::ok(0);
    }

    // Handle null or empty data
    if (data == nullptr || length == 0) {
        return HandshakeResult::ok(0);
    }

    switch (state_) {
        case HandshakeState::WaitingC0:
            return processC0(data, length);
        case HandshakeState::WaitingC1:
            return processC1(data, length);
        case HandshakeState::WaitingC2:
            return processC2(data, length);
        default:
            return HandshakeResult::ok(0);
    }
}

HandshakeResult HandshakeHandler::processC0(const uint8_t* data, size_t length) {
    // Need at least 1 byte for C0
    if (length < handshake::C0_SIZE) {
        return HandshakeResult::ok(0);
    }

    // Validate version byte (must be 3)
    uint8_t version = data[0];
    if (version != handshake::RTMP_VERSION) {
        state_ = HandshakeState::Failed;
        return HandshakeResult::fail(HandshakeError{
            HandshakeError::Code::InvalidVersion,
            "Invalid RTMP version: expected 3, got " + std::to_string(version)
        });
    }

    // Transition to WaitingC1
    state_ = HandshakeState::WaitingC1;
    return HandshakeResult::ok(handshake::C0_SIZE);
}

HandshakeResult HandshakeHandler::processC1(const uint8_t* data, size_t length) {
    // Need 1536 bytes for C1
    if (length < handshake::C1_SIZE) {
        return HandshakeResult::ok(0);
    }

    // Store C1 data for S2 generation
    c1Data_.assign(data, data + handshake::C1_SIZE);

    // Generate S0+S1+S2 response
    generateResponse();

    // Transition to WaitingC2
    state_ = HandshakeState::WaitingC2;
    return HandshakeResult::ok(handshake::C1_SIZE);
}

HandshakeResult HandshakeHandler::processC2(const uint8_t* data, size_t length) {
    // Need 1536 bytes for C2
    if (length < handshake::C2_SIZE) {
        return HandshakeResult::ok(0);
    }

    // Validate C2 echoes S1 correctly
    if (!validateC2Echo(data)) {
        state_ = HandshakeState::Failed;
        return HandshakeResult::fail(HandshakeError{
            HandshakeError::Code::MalformedPacket,
            "C2 validation failed: S1 echo mismatch"
        });
    }

    // Handshake complete
    state_ = HandshakeState::Complete;
    return HandshakeResult::ok(handshake::C2_SIZE);
}

void HandshakeHandler::generateResponse() {
    // Clear any existing response
    responseData_.clear();

    // Reserve space for S0 + S1 + S2
    responseData_.reserve(handshake::S0_SIZE + handshake::S1_SIZE + handshake::S2_SIZE);

    core::Buffer response;
    core::BufferWriter writer(response);

    // S0: Version byte (1 byte)
    writer.writeUint8(handshake::RTMP_VERSION);

    // S1: Already generated in constructor (1536 bytes)
    writer.writeBytes(s1Data_.data(), s1Data_.size());

    // S2: Echo of C1 (1536 bytes)
    // Structure: C1 timestamp (4) + server time when C1 was read (4) + C1 random data (1528)

    // Extract C1 timestamp (first 4 bytes of C1)
    if (c1Data_.size() >= handshake::TIMESTAMP_SIZE) {
        writer.writeBytes(c1Data_.data(), handshake::TIMESTAMP_SIZE);
    } else {
        writer.writeUint32BE(0);
    }

    // Server timestamp when C1 was read
    writer.writeUint32BE(getCurrentTimestamp());

    // Echo C1 random data (bytes 8-1535 of C1)
    if (c1Data_.size() >= handshake::C1_SIZE) {
        writer.writeBytes(c1Data_.data() + handshake::TIMESTAMP_SIZE + handshake::ZERO_SIZE,
                         handshake::RANDOM_SIZE);
    } else {
        // Fill with zeros if C1 data is insufficient
        std::vector<uint8_t> zeros(handshake::RANDOM_SIZE, 0);
        writer.writeBytes(zeros);
    }

    // Move response data
    responseData_ = response.vector();
}

bool HandshakeHandler::validateC2Echo(const uint8_t* c2Data) const {
    // C2 should echo S1: S1 timestamp (4) + time C1 was read (4) + S1 random (1528)
    // We validate that the first 4 bytes match S1 timestamp
    // and the random data portion (bytes 8-1535) matches S1 random data

    // Check S1 timestamp echo (first 4 bytes of C2 should match first 4 bytes of S1)
    if (std::memcmp(c2Data, s1Data_.data(), handshake::TIMESTAMP_SIZE) != 0) {
        return false;
    }

    // Check S1 random data echo (bytes 8-1535 of C2 should match bytes 8-1535 of S1)
    const uint8_t* c2Random = c2Data + handshake::TIMESTAMP_SIZE + handshake::ZERO_SIZE;
    const uint8_t* s1Random = s1Data_.data() + handshake::TIMESTAMP_SIZE + handshake::ZERO_SIZE;

    if (std::memcmp(c2Random, s1Random, handshake::RANDOM_SIZE) != 0) {
        return false;
    }

    return true;
}

std::vector<uint8_t> HandshakeHandler::getResponseData() {
    std::vector<uint8_t> result;
    std::swap(result, responseData_);
    return result;
}

HandshakeState HandshakeHandler::getState() const {
    return state_;
}

bool HandshakeHandler::isComplete() const {
    return state_ == HandshakeState::Complete;
}

uint32_t HandshakeHandler::getCurrentTimestamp() const {
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    return static_cast<uint32_t>(millis & 0xFFFFFFFF);
}

void HandshakeHandler::generateRandomBytes(uint8_t* buffer, size_t length) {
    bool success = false;

#if defined(_WIN32) || defined(_WIN64)
    // Windows: Use BCryptGenRandom
    NTSTATUS status = BCryptGenRandom(
        NULL,
        buffer,
        static_cast<ULONG>(length),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG
    );
    success = (status == 0);  // STATUS_SUCCESS

#elif defined(__APPLE__)
    // macOS/iOS: Use SecRandomCopyBytes
    int result = SecRandomCopyBytes(kSecRandomDefault, length, buffer);
    success = (result == errSecSuccess);

#elif defined(__linux__) || defined(__ANDROID__)
    // Linux/Android: Use getrandom()
    ssize_t result = getrandom(buffer, length, 0);
    success = (result == static_cast<ssize_t>(length));
#endif

    // Fallback to std::random_device if platform-specific method fails
    if (!success) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);

        for (size_t i = 0; i < length; ++i) {
            buffer[i] = static_cast<uint8_t>(dis(gen));
        }
    }
}

} // namespace protocol
} // namespace openrtmp
