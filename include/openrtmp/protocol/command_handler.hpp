// OpenRTMP - Cross-platform RTMP Server
// Command Handler - RTMP command processing
//
// This component handles RTMP commands (AMF-encoded) and manages stream operations.
// It processes connect, createStream, publish, play, deleteStream, and closeStream
// commands according to the RTMP specification.
//
// Requirements coverage:
// - Requirement 3.1: connect command response within 50ms
// - Requirement 3.2: createStream command with stream ID allocation
// - Requirement 3.3: publish command with stream key validation
// - Requirement 3.4: play command for subscription
// - Requirement 3.5: deleteStream for resource release
// - Requirement 3.6: closeStream for stream stop
// - Requirement 3.7: Stream key conflict detection

#ifndef OPENRTMP_PROTOCOL_COMMAND_HANDLER_HPP
#define OPENRTMP_PROTOCOL_COMMAND_HANDLER_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <variant>

#include "openrtmp/core/result.hpp"
#include "openrtmp/core/types.hpp"
#include "openrtmp/protocol/amf_codec.hpp"
#include "openrtmp/streaming/stream_registry.hpp"

namespace openrtmp {
namespace protocol {

// =============================================================================
// Session State Types
// =============================================================================

/**
 * Session state enumeration
 * Tracks the current state of an RTMP session
 */
enum class SessionState {
    Initial,      // Connection accepted, handshake pending
    Handshaking,  // Handshake in progress
    Connected,    // Handshake complete, ready for commands
    Publishing,   // Actively publishing a stream
    Subscribing,  // Actively subscribing to a stream
    Closed        // Session closed
};

/**
 * Session context
 * Maintains state information for an RTMP session
 */
struct SessionContext {
    SessionId sessionId{0};           // Unique session identifier
    ConnectionId connectionId{0};     // Associated connection ID
    SessionState state{SessionState::Initial};
    std::string appName;              // Connected application name
    StreamId streamId{0};             // Currently active stream ID
    std::string streamKey;            // Currently active stream key (for publish/play)

    bool isConnected() const { return !appName.empty() && state >= SessionState::Connected; }
    bool hasStream() const { return streamId != 0; }
};

// =============================================================================
// Command Types
// =============================================================================

/**
 * RTMP Command structure
 * Represents a decoded RTMP command message
 */
struct RTMPCommand {
    std::string name;                 // Command name (connect, createStream, etc.)
    double transactionId{0.0};        // Transaction ID for request/response matching
    AMFValue commandObject;           // First parameter (usually object)
    std::vector<AMFValue> args;       // Additional parameters
};

/**
 * Command response message
 * Represents a response to be sent back to the client
 */
struct CommandResponseMessage {
    std::string commandName;          // _result, _error, onStatus, etc.
    double transactionId{0.0};        // Matching transaction ID
    AMFValue commandObject;           // Response command object
    std::vector<AMFValue> args;       // Additional response arguments
    StreamId streamId{0};             // Stream ID (for createStream response)
    std::string statusCode;           // Status code (for onStatus responses)
    std::string statusDescription;    // Status description
    uint32_t messageStreamId{0};      // Message stream ID for the response
};

/**
 * Command result
 * Contains the result of processing a command
 */
struct CommandResult {
    std::vector<CommandResponseMessage> responses;  // Messages to send back
};

/**
 * Command error codes
 */
enum class CommandErrorCode {
    None,
    InvalidApp,           // Invalid application name
    StreamKeyInUse,       // Stream key already being published
    StreamNotFound,       // Requested stream not found
    AuthFailed,           // Authentication failed
    NotConnected,         // Command requires connected state
    InvalidState,         // Command not valid in current state
    InvalidStreamId,      // Invalid stream ID
    InvalidArguments,     // Invalid command arguments
    InternalError         // Internal processing error
};

/**
 * Command error
 */
struct CommandError {
    CommandErrorCode code;
    std::string description;

    CommandError(CommandErrorCode c, std::string desc)
        : code(c), description(std::move(desc)) {}
};

// =============================================================================
// Command Handler Interface
// =============================================================================

/**
 * ICommandHandler interface
 * Abstract interface for RTMP command processing
 */
class ICommandHandler {
public:
    virtual ~ICommandHandler() = default;

    /**
     * Process a generic RTMP command
     * Routes to appropriate handler based on command name
     *
     * @param command The decoded RTMP command
     * @param session The session context (modified in place)
     * @return Result containing command responses or error
     */
    virtual core::Result<CommandResult, CommandError> processCommand(
        const RTMPCommand& command,
        SessionContext& session
    ) = 0;

    /**
     * Handle connect command
     * Validates application name and establishes RTMP session
     *
     * @param command The connect command
     * @param session The session context
     * @return Result containing _result or _error response
     */
    virtual core::Result<CommandResult, CommandError> handleConnect(
        const RTMPCommand& command,
        SessionContext& session
    ) = 0;

    /**
     * Handle createStream command
     * Allocates a new stream ID for the session
     *
     * @param command The createStream command
     * @param session The session context
     * @return Result containing stream ID in _result response
     */
    virtual core::Result<CommandResult, CommandError> handleCreateStream(
        const RTMPCommand& command,
        SessionContext& session
    ) = 0;

    /**
     * Handle publish command
     * Begins publishing a stream with the specified key
     *
     * @param command The publish command (args[0] = stream name, args[1] = type)
     * @param session The session context
     * @return Result containing onStatus response
     */
    virtual core::Result<CommandResult, CommandError> handlePublish(
        const RTMPCommand& command,
        SessionContext& session
    ) = 0;

    /**
     * Handle play command
     * Begins subscribing to a stream
     *
     * @param command The play command (args[0] = stream name)
     * @param session The session context
     * @return Result containing onStatus response
     */
    virtual core::Result<CommandResult, CommandError> handlePlay(
        const RTMPCommand& command,
        SessionContext& session
    ) = 0;

    /**
     * Handle deleteStream command
     * Releases stream resources and stream ID
     *
     * @param command The deleteStream command
     * @param session The session context
     * @return Result containing command result
     */
    virtual core::Result<CommandResult, CommandError> handleDeleteStream(
        const RTMPCommand& command,
        SessionContext& session
    ) = 0;

    /**
     * Handle closeStream command
     * Stops the stream but maintains the connection for reuse
     *
     * @param command The closeStream command
     * @param session The session context
     * @return Result containing command result
     */
    virtual core::Result<CommandResult, CommandError> handleCloseStream(
        const RTMPCommand& command,
        SessionContext& session
    ) = 0;
};

// =============================================================================
// Command Handler Implementation
// =============================================================================

/**
 * CommandHandler implementation
 * Processes RTMP commands using AMFCodec for encoding/decoding
 * and StreamRegistry for stream management
 */
class CommandHandler : public ICommandHandler {
public:
    /**
     * Constructor
     *
     * @param streamRegistry Shared stream registry for stream management
     * @param amfCodec Shared AMF codec for encoding/decoding
     */
    CommandHandler(
        std::shared_ptr<streaming::StreamRegistry> streamRegistry,
        std::shared_ptr<IAMFCodec> amfCodec
    );

    ~CommandHandler() override = default;

    // Non-copyable
    CommandHandler(const CommandHandler&) = delete;
    CommandHandler& operator=(const CommandHandler&) = delete;

    // Movable
    CommandHandler(CommandHandler&&) = default;
    CommandHandler& operator=(CommandHandler&&) = default;

    // ICommandHandler implementation
    core::Result<CommandResult, CommandError> processCommand(
        const RTMPCommand& command,
        SessionContext& session
    ) override;

    core::Result<CommandResult, CommandError> handleConnect(
        const RTMPCommand& command,
        SessionContext& session
    ) override;

    core::Result<CommandResult, CommandError> handleCreateStream(
        const RTMPCommand& command,
        SessionContext& session
    ) override;

    core::Result<CommandResult, CommandError> handlePublish(
        const RTMPCommand& command,
        SessionContext& session
    ) override;

    core::Result<CommandResult, CommandError> handlePlay(
        const RTMPCommand& command,
        SessionContext& session
    ) override;

    core::Result<CommandResult, CommandError> handleDeleteStream(
        const RTMPCommand& command,
        SessionContext& session
    ) override;

    core::Result<CommandResult, CommandError> handleCloseStream(
        const RTMPCommand& command,
        SessionContext& session
    ) override;

private:
    // Helper methods
    CommandResponseMessage createResultResponse(
        double transactionId,
        const AMFValue& properties,
        const AMFValue& information
    ) const;

    CommandResponseMessage createErrorResponse(
        double transactionId,
        const std::string& code,
        const std::string& description
    ) const;

    CommandResponseMessage createOnStatusResponse(
        const std::string& level,
        const std::string& code,
        const std::string& description,
        uint32_t messageStreamId = 0
    ) const;

    std::string extractAppName(const AMFValue& commandObject) const;
    std::string extractStreamName(const std::vector<AMFValue>& args) const;
    StreamId extractStreamIdArg(const std::vector<AMFValue>& args) const;

    bool validateAppName(const std::string& appName) const;

    std::shared_ptr<streaming::StreamRegistry> streamRegistry_;
    std::shared_ptr<IAMFCodec> amfCodec_;
};

} // namespace protocol
} // namespace openrtmp

#endif // OPENRTMP_PROTOCOL_COMMAND_HANDLER_HPP
