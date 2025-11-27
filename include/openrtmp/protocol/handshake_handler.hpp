// OpenRTMP - Cross-platform RTMP Server
// RTMP Handshake Handler - State Machine for RTMP Handshake Protocol
//
// Implements the RTMP handshake protocol per Adobe specification:
// - C0/S0: Version exchange (1 byte, must be 3)
// - C1/S1: Random data exchange (1536 bytes: 4-byte timestamp + 4-byte zero + 1528-byte random)
// - C2/S2: Echo exchange (1536 bytes: echoed timestamp + time read + echoed random)
//
// State transitions: WaitingC0 -> WaitingC1 -> WaitingC2 -> Complete | Failed

#ifndef OPENRTMP_PROTOCOL_HANDSHAKE_HANDLER_HPP
#define OPENRTMP_PROTOCOL_HANDSHAKE_HANDLER_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <optional>
#include <string>
#include <memory>

#include "openrtmp/core/buffer.hpp"

namespace openrtmp {
namespace protocol {

// RTMP Handshake Protocol Constants
namespace handshake {
    constexpr uint8_t RTMP_VERSION = 3;      // RTMP version 3
    constexpr size_t C0_SIZE = 1;            // C0 packet size
    constexpr size_t C1_SIZE = 1536;         // C1 packet size
    constexpr size_t C2_SIZE = 1536;         // C2 packet size
    constexpr size_t S0_SIZE = 1;            // S0 packet size
    constexpr size_t S1_SIZE = 1536;         // S1 packet size
    constexpr size_t S2_SIZE = 1536;         // S2 packet size
    constexpr size_t TIMESTAMP_SIZE = 4;     // Timestamp field size
    constexpr size_t ZERO_SIZE = 4;          // Zero field size
    constexpr size_t RANDOM_SIZE = 1528;     // Random data size (1536 - 4 - 4)
}

/**
 * @brief Handshake state machine states.
 *
 * States progress unidirectionally:
 * WaitingC0 -> WaitingC1 -> WaitingC2 -> Complete
 *           -> Failed (terminal state from any state on error)
 */
enum class HandshakeState {
    WaitingC0,   ///< Initial state, waiting for C0 (version byte)
    WaitingC1,   ///< Received C0, waiting for C1 (1536 bytes)
    WaitingC2,   ///< Received C1, sent S0+S1+S2, waiting for C2 (1536 bytes)
    Complete,    ///< Handshake completed successfully
    Failed       ///< Handshake failed (terminal state)
};

/**
 * @brief Handshake error information.
 */
struct HandshakeError {
    /**
     * @brief Error codes for handshake failures.
     */
    enum class Code {
        InvalidVersion,    ///< C0 version byte is not 3
        MalformedPacket,   ///< Packet structure is invalid
        SequenceError,     ///< Packets received out of order
        Timeout            ///< Handshake timeout exceeded
    };

    Code code;             ///< Error code
    std::string message;   ///< Human-readable error message

    HandshakeError(Code c = Code::MalformedPacket, std::string msg = "")
        : code(c), message(std::move(msg)) {}
};

/**
 * @brief Result of processing handshake data.
 */
struct HandshakeResult {
    bool success;                              ///< Whether processing succeeded
    size_t bytesConsumed;                      ///< Number of bytes consumed from input
    std::optional<HandshakeError> error;       ///< Error information if failed

    /**
     * @brief Create a successful result.
     * @param consumed Number of bytes consumed
     * @return Successful HandshakeResult
     */
    static HandshakeResult ok(size_t consumed) {
        return HandshakeResult{true, consumed, std::nullopt};
    }

    /**
     * @brief Create a failed result.
     * @param err Error information
     * @return Failed HandshakeResult
     */
    static HandshakeResult fail(HandshakeError err) {
        return HandshakeResult{false, 0, std::move(err)};
    }
};

/**
 * @brief Interface for handshake handler operations.
 *
 * This interface defines the contract for RTMP handshake processing.
 */
class IHandshakeHandler {
public:
    virtual ~IHandshakeHandler() = default;

    /**
     * @brief Process incoming handshake data.
     *
     * Processes data according to current state:
     * - WaitingC0: Expects 1 byte (version), validates it's 3
     * - WaitingC1: Expects 1536 bytes, generates S0+S1+S2 response
     * - WaitingC2: Expects 1536 bytes, validates echo of S1
     * - Complete/Failed: Ignores further data
     *
     * @param data Pointer to input data
     * @param length Length of input data
     * @return HandshakeResult indicating success/failure and bytes consumed
     */
    virtual HandshakeResult processData(const uint8_t* data, size_t length) = 0;

    /**
     * @brief Get response data to send to client.
     *
     * After processing C1, returns S0+S1+S2 (3073 bytes total).
     * Subsequent calls return empty until new response is generated.
     *
     * @return Response data to send
     */
    virtual std::vector<uint8_t> getResponseData() = 0;

    /**
     * @brief Get current handshake state.
     * @return Current HandshakeState
     */
    virtual HandshakeState getState() const = 0;

    /**
     * @brief Check if handshake is complete.
     * @return true if state is Complete
     */
    virtual bool isComplete() const = 0;
};

/**
 * @brief RTMP Handshake Handler implementation.
 *
 * Implements the RTMP handshake state machine per Adobe specification:
 *
 * Protocol flow:
 * 1. Client sends C0 (version byte, must be 3)
 * 2. Server validates C0, transitions to WaitingC1
 * 3. Client sends C1 (timestamp + zero + 1528 random bytes)
 * 4. Server sends S0+S1+S2, transitions to WaitingC2
 *    - S0: Version byte (3)
 *    - S1: Server timestamp + zero + 1528 random bytes
 *    - S2: Echo of C1 (C1 timestamp + server time + C1 random data)
 * 5. Client sends C2 (echo of S1)
 * 6. Server validates C2, transitions to Complete
 *
 * Requirements coverage:
 * - Requirement 1.2: S0+S1 response within 100ms
 * - Requirement 1.3: S2 echoes C1 timestamp and random data
 * - Requirement 1.4: C2 validation with timestamp verification
 */
class HandshakeHandler : public IHandshakeHandler {
public:
    /**
     * @brief Construct a new HandshakeHandler.
     *
     * Creates a handler in WaitingC0 state with cryptographically
     * random S1 data pre-generated.
     */
    HandshakeHandler();

    /**
     * @brief Destructor.
     */
    ~HandshakeHandler() override = default;

    // Non-copyable but movable
    HandshakeHandler(const HandshakeHandler&) = delete;
    HandshakeHandler& operator=(const HandshakeHandler&) = delete;
    HandshakeHandler(HandshakeHandler&&) noexcept = default;
    HandshakeHandler& operator=(HandshakeHandler&&) noexcept = default;

    // IHandshakeHandler interface
    HandshakeResult processData(const uint8_t* data, size_t length) override;
    std::vector<uint8_t> getResponseData() override;
    HandshakeState getState() const override;
    bool isComplete() const override;

private:
    /**
     * @brief Process C0 packet (version byte).
     * @param data Pointer to data
     * @param length Data length
     * @return HandshakeResult
     */
    HandshakeResult processC0(const uint8_t* data, size_t length);

    /**
     * @brief Process C1 packet (1536 bytes).
     * @param data Pointer to data
     * @param length Data length
     * @return HandshakeResult
     */
    HandshakeResult processC1(const uint8_t* data, size_t length);

    /**
     * @brief Process C2 packet (1536 bytes).
     * @param data Pointer to data
     * @param length Data length
     * @return HandshakeResult
     */
    HandshakeResult processC2(const uint8_t* data, size_t length);

    /**
     * @brief Generate S0+S1+S2 response.
     *
     * Generates:
     * - S0: Version byte (3)
     * - S1: Server timestamp + zero + 1528 random bytes
     * - S2: C1 timestamp + server timestamp + C1 random data
     */
    void generateResponse();

    /**
     * @brief Generate cryptographically random bytes.
     * @param buffer Output buffer
     * @param length Number of bytes to generate
     */
    void generateRandomBytes(uint8_t* buffer, size_t length);

    /**
     * @brief Get current timestamp in milliseconds.
     * @return Current timestamp
     */
    uint32_t getCurrentTimestamp() const;

    /**
     * @brief Validate C2 echoes S1 correctly.
     * @param c2Data C2 packet data
     * @return true if validation passes
     */
    bool validateC2Echo(const uint8_t* c2Data) const;

    HandshakeState state_;              ///< Current state
    std::vector<uint8_t> responseData_; ///< Pending response data
    std::vector<uint8_t> s1Data_;       ///< S1 packet data (for C2 validation)
    std::vector<uint8_t> c1Data_;       ///< Stored C1 data for S2 generation
    uint32_t serverTimestamp_;          ///< Server timestamp when S1 was generated
};

} // namespace protocol
} // namespace openrtmp

#endif // OPENRTMP_PROTOCOL_HANDSHAKE_HANDLER_HPP
