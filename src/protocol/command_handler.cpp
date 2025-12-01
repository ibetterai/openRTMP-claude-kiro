// OpenRTMP - Cross-platform RTMP Server
// Command Handler Implementation
//
// Implements RTMP command processing (connect, createStream, publish, play,
// deleteStream, closeStream) using AMFCodec for encoding/decoding and
// StreamRegistry for stream management.
//
// Requirements coverage:
// - Requirement 3.1: connect command response within 50ms
// - Requirement 3.2: createStream command with stream ID allocation
// - Requirement 3.3: publish command with stream key validation
// - Requirement 3.4: play command for subscription
// - Requirement 3.5: deleteStream for resource release
// - Requirement 3.6: closeStream for stream stop
// - Requirement 3.7: Stream key conflict detection

#include "openrtmp/protocol/command_handler.hpp"

#include <algorithm>
#include <iostream>
#include <mutex>

namespace openrtmp {
namespace protocol {

// =============================================================================
// Constructor
// =============================================================================

CommandHandler::CommandHandler(
    std::shared_ptr<streaming::StreamRegistry> streamRegistry,
    std::shared_ptr<IAMFCodec> amfCodec
)
    : streamRegistry_(std::move(streamRegistry))
    , amfCodec_(std::move(amfCodec))
{
}

// =============================================================================
// Command Processing
// =============================================================================

core::Result<CommandResult, CommandError> CommandHandler::processCommand(
    const RTMPCommand& command,
    SessionContext& session
) {
    // Route to appropriate handler based on command name
    if (command.name == "connect") {
        return handleConnect(command, session);
    } else if (command.name == "createStream") {
        return handleCreateStream(command, session);
    } else if (command.name == "publish") {
        return handlePublish(command, session);
    } else if (command.name == "play") {
        return handlePlay(command, session);
    } else if (command.name == "deleteStream") {
        return handleDeleteStream(command, session);
    } else if (command.name == "closeStream") {
        return handleCloseStream(command, session);
    } else if (command.name == "releaseStream" ||
               command.name == "FCPublish" ||
               command.name == "FCUnpublish") {
        // These are NetConnection commands that don't require a response
        // or just need a simple acknowledgment (null _result)
        // releaseStream: Client is releasing a stream name before publishing
        // FCPublish: Flash Media Server publish notification
        // FCUnpublish: Flash Media Server unpublish notification
        CommandResult result;
        // These commands typically don't expect a response, but if they do,
        // we return _result with null command object
        CommandResponseMessage response;
        response.commandName = "_result";
        response.transactionId = command.transactionId;
        response.commandObject.type = AMFValue::Type::Null;
        // No additional args needed
        result.responses.push_back(response);
        return core::Result<CommandResult, CommandError>::success(std::move(result));
    } else if (command.name == "@setDataFrame" ||
               command.name == "onMetaData" ||
               command.name == "|RtmpSampleAccess") {
        // Data/metadata commands - acknowledge without response
        CommandResult result;
        // No response needed for data commands
        return core::Result<CommandResult, CommandError>::success(std::move(result));
    } else {
        // Unknown command - log and return empty result (don't disconnect)
        // Many RTMP clients send various proprietary commands that can be ignored
        CommandResult result;
        // Return empty result - no response sent, connection continues
        return core::Result<CommandResult, CommandError>::success(std::move(result));
    }
}

// =============================================================================
// Connect Command Handler
// =============================================================================

core::Result<CommandResult, CommandError> CommandHandler::handleConnect(
    const RTMPCommand& command,
    SessionContext& session
) {
    CommandResult result;

    // Check if already connected (state validation)
    if (!session.appName.empty() && session.state >= SessionState::Connected) {
        result.responses.push_back(createErrorResponse(
            command.transactionId,
            "NetConnection.Connect.Rejected",
            "Already connected to application"
        ));
        return core::Result<CommandResult, CommandError>::success(std::move(result));
    }

    // Extract and validate application name
    std::string appName = extractAppName(command.commandObject);

    if (!validateAppName(appName)) {
        result.responses.push_back(createErrorResponse(
            command.transactionId,
            "NetConnection.Connect.InvalidApp",
            "Invalid application name"
        ));
        return core::Result<CommandResult, CommandError>::success(std::move(result));
    }

    // Update session state
    session.appName = appName;
    session.state = SessionState::Connected;

    // Create success response
    // Properties object
    std::map<std::string, AMFValue> properties;

    AMFValue fmsVer;
    fmsVer.type = AMFValue::Type::String;
    fmsVer.data = std::string("FMS/5,0,0,1");
    properties["fmsVer"] = fmsVer;

    AMFValue capabilities;
    capabilities.type = AMFValue::Type::Number;
    capabilities.data = 255.0;
    properties["capabilities"] = capabilities;

    AMFValue mode;
    mode.type = AMFValue::Type::Number;
    mode.data = 1.0;
    properties["mode"] = mode;

    AMFValue propertiesValue;
    propertiesValue.type = AMFValue::Type::Object;
    propertiesValue.data = properties;

    // Information object
    std::map<std::string, AMFValue> information;

    AMFValue level;
    level.type = AMFValue::Type::String;
    level.data = std::string("status");
    information["level"] = level;

    AMFValue code;
    code.type = AMFValue::Type::String;
    code.data = std::string("NetConnection.Connect.Success");
    information["code"] = code;

    AMFValue description;
    description.type = AMFValue::Type::String;
    description.data = std::string("Connection succeeded.");
    information["description"] = description;

    AMFValue objectEncoding;
    objectEncoding.type = AMFValue::Type::Number;
    objectEncoding.data = 0.0;  // AMF0
    information["objectEncoding"] = objectEncoding;

    AMFValue informationValue;
    informationValue.type = AMFValue::Type::Object;
    informationValue.data = information;

    result.responses.push_back(createResultResponse(
        command.transactionId,
        propertiesValue,
        informationValue
    ));

    return core::Result<CommandResult, CommandError>::success(std::move(result));
}

// =============================================================================
// CreateStream Command Handler
// =============================================================================

core::Result<CommandResult, CommandError> CommandHandler::handleCreateStream(
    const RTMPCommand& command,
    SessionContext& session
) {
    CommandResult result;

    // Validate session state - must be connected
    if (session.appName.empty()) {
        result.responses.push_back(createErrorResponse(
            command.transactionId,
            "NetStream.Failed",
            "Not connected to application"
        ));
        return core::Result<CommandResult, CommandError>::success(std::move(result));
    }

    // Allocate stream ID
    auto streamIdResult = streamRegistry_->allocateStreamId();
    if (streamIdResult.isError()) {
        result.responses.push_back(createErrorResponse(
            command.transactionId,
            "NetStream.Failed",
            "Failed to allocate stream ID"
        ));
        return core::Result<CommandResult, CommandError>::success(std::move(result));
    }

    StreamId streamId = streamIdResult.value();
    session.streamId = streamId;

    // Create success response with stream ID
    CommandResponseMessage response;
    response.commandName = "_result";
    response.transactionId = command.transactionId;
    response.commandObject = AMFValue::makeNull();
    response.streamId = streamId;

    // The stream ID is returned as the second argument (after null command object)
    AMFValue streamIdValue;
    streamIdValue.type = AMFValue::Type::Number;
    streamIdValue.data = static_cast<double>(streamId);
    response.args.push_back(streamIdValue);

    result.responses.push_back(std::move(response));

    return core::Result<CommandResult, CommandError>::success(std::move(result));
}

// =============================================================================
// Publish Command Handler
// =============================================================================

core::Result<CommandResult, CommandError> CommandHandler::handlePublish(
    const RTMPCommand& command,
    SessionContext& session
) {
    CommandResult result;

    // Validate session state - must have stream ID
    if (session.streamId == 0) {
        result.responses.push_back(createOnStatusResponse(
            "error",
            "NetStream.Publish.Failed",
            "No stream created",
            session.streamId
        ));
        return core::Result<CommandResult, CommandError>::success(std::move(result));
    }

    // Extract stream name from arguments
    std::string streamName = extractStreamName(command.args);

    if (streamName.empty()) {
        result.responses.push_back(createOnStatusResponse(
            "error",
            "NetStream.Publish.BadName",
            "Invalid stream name",
            session.streamId
        ));
        return core::Result<CommandResult, CommandError>::success(std::move(result));
    }

    // Create stream key
    StreamKey streamKey(session.appName, streamName);

    // Attempt to register stream (checks for conflicts - Requirement 3.7)
    auto registerResult = streamRegistry_->registerStream(
        streamKey,
        session.streamId,
        static_cast<PublisherId>(session.connectionId)
    );

    if (registerResult.isError()) {
        // Stream key already in use
        result.responses.push_back(createOnStatusResponse(
            "error",
            "NetStream.Publish.BadName",
            "Stream key already in use",
            session.streamId
        ));
        return core::Result<CommandResult, CommandError>::success(std::move(result));
    }

    // Update session state
    session.state = SessionState::Publishing;
    session.streamKey = streamName;

    // Create success response
    result.responses.push_back(createOnStatusResponse(
        "status",
        "NetStream.Publish.Start",
        "Publishing " + streamName,
        session.streamId
    ));

    return core::Result<CommandResult, CommandError>::success(std::move(result));
}

// =============================================================================
// Play Command Handler
// =============================================================================

core::Result<CommandResult, CommandError> CommandHandler::handlePlay(
    const RTMPCommand& command,
    SessionContext& session
) {
    CommandResult result;

    // Validate session state - must have stream ID
    if (session.streamId == 0) {
        result.responses.push_back(createOnStatusResponse(
            "error",
            "NetStream.Play.Failed",
            "No stream created",
            session.streamId
        ));
        return core::Result<CommandResult, CommandError>::success(std::move(result));
    }

    // Extract stream name from arguments
    std::string streamName = extractStreamName(command.args);

    if (streamName.empty()) {
        result.responses.push_back(createOnStatusResponse(
            "error",
            "NetStream.Play.Failed",
            "Invalid stream name",
            session.streamId
        ));
        return core::Result<CommandResult, CommandError>::success(std::move(result));
    }

    // Create stream key
    StreamKey streamKey(session.appName, streamName);

    // DEBUG: Log stream key lookup
    std::cerr << "[DEBUG] handlePlay: Looking for stream key: appName='" << session.appName
              << "' streamName='" << streamName << "'" << std::endl;
    std::cerr << "[DEBUG] handlePlay: Active streams count: " << streamRegistry_->getActiveStreamCount() << std::endl;
    auto allKeys = streamRegistry_->getAllStreamKeys();
    for (const auto& key : allKeys) {
        std::cerr << "[DEBUG] handlePlay: Registered stream: '" << key.toString() << "'" << std::endl;
    }

    // Check if stream exists
    if (!streamRegistry_->hasStream(streamKey)) {
        std::cerr << "[DEBUG] handlePlay: Stream NOT FOUND - key='" << streamKey.toString() << "'" << std::endl;
        result.responses.push_back(createOnStatusResponse(
            "error",
            "NetStream.Play.StreamNotFound",
            "Stream not found: " + streamName,
            session.streamId
        ));
        return core::Result<CommandResult, CommandError>::success(std::move(result));
    }
    std::cerr << "[DEBUG] handlePlay: Stream FOUND - key='" << streamKey.toString() << "'" << std::endl;

    // Add subscriber to stream
    auto addResult = streamRegistry_->addSubscriber(
        streamKey,
        static_cast<SubscriberId>(session.connectionId)
    );

    if (addResult.isError()) {
        result.responses.push_back(createOnStatusResponse(
            "error",
            "NetStream.Play.Failed",
            "Failed to subscribe to stream",
            session.streamId
        ));
        return core::Result<CommandResult, CommandError>::success(std::move(result));
    }

    // Update session state
    session.state = SessionState::Subscribing;
    session.streamKey = streamName;

    // Create success response
    // First send StreamIsRecorded (User Control Message) - optional
    // Then send StreamBegin (User Control Message) - optional

    // Send onStatus NetStream.Play.Reset
    result.responses.push_back(createOnStatusResponse(
        "status",
        "NetStream.Play.Reset",
        "Playing and resetting " + streamName,
        session.streamId
    ));

    // Send onStatus NetStream.Play.Start
    result.responses.push_back(createOnStatusResponse(
        "status",
        "NetStream.Play.Start",
        "Started playing " + streamName,
        session.streamId
    ));

    return core::Result<CommandResult, CommandError>::success(std::move(result));
}

// =============================================================================
// DeleteStream Command Handler
// =============================================================================

core::Result<CommandResult, CommandError> CommandHandler::handleDeleteStream(
    const RTMPCommand& command,
    SessionContext& session
) {
    CommandResult result;

    // Extract stream ID from arguments (if provided)
    StreamId targetStreamId = extractStreamIdArg(command.args);

    // If no stream ID in args, use session's stream ID
    if (targetStreamId == 0) {
        targetStreamId = session.streamId;
    }

    // If still no valid stream ID, just return success (nothing to delete)
    if (targetStreamId == 0) {
        return core::Result<CommandResult, CommandError>::success(std::move(result));
    }

    // If session was publishing, unregister the stream
    if (session.state == SessionState::Publishing && !session.streamKey.empty()) {
        StreamKey streamKey(session.appName, session.streamKey);
        streamRegistry_->unregisterStream(streamKey);
    }

    // If session was subscribing, remove subscriber
    if (session.state == SessionState::Subscribing && !session.streamKey.empty()) {
        StreamKey streamKey(session.appName, session.streamKey);
        streamRegistry_->removeSubscriber(
            streamKey,
            static_cast<SubscriberId>(session.connectionId)
        );
    }

    // Release stream ID
    streamRegistry_->releaseStreamId(targetStreamId);

    // Update session state
    session.state = SessionState::Connected;
    session.streamId = 0;
    session.streamKey.clear();

    return core::Result<CommandResult, CommandError>::success(std::move(result));
}

// =============================================================================
// CloseStream Command Handler
// =============================================================================

core::Result<CommandResult, CommandError> CommandHandler::handleCloseStream(
    const RTMPCommand& command,
    SessionContext& session
) {
    CommandResult result;

    // If session was publishing, unregister the stream
    if (session.state == SessionState::Publishing && !session.streamKey.empty()) {
        StreamKey streamKey(session.appName, session.streamKey);
        streamRegistry_->unregisterStream(streamKey);
    }

    // If session was subscribing, remove subscriber
    if (session.state == SessionState::Subscribing && !session.streamKey.empty()) {
        StreamKey streamKey(session.appName, session.streamKey);
        streamRegistry_->removeSubscriber(
            streamKey,
            static_cast<SubscriberId>(session.connectionId)
        );
    }

    // Update session state - connection remains, but stream is closed
    session.state = SessionState::Connected;
    session.streamKey.clear();
    // Note: streamId is NOT cleared - can be reused

    return core::Result<CommandResult, CommandError>::success(std::move(result));
}

// =============================================================================
// Helper Methods
// =============================================================================

CommandResponseMessage CommandHandler::createResultResponse(
    double transactionId,
    const AMFValue& properties,
    const AMFValue& information
) const {
    CommandResponseMessage response;
    response.commandName = "_result";
    response.transactionId = transactionId;
    response.commandObject = properties;
    response.args.push_back(information);
    return response;
}

CommandResponseMessage CommandHandler::createErrorResponse(
    double transactionId,
    const std::string& code,
    const std::string& description
) const {
    CommandResponseMessage response;
    response.commandName = "_error";
    response.transactionId = transactionId;

    // Create error object
    std::map<std::string, AMFValue> errorObj;

    AMFValue levelVal;
    levelVal.type = AMFValue::Type::String;
    levelVal.data = std::string("error");
    errorObj["level"] = levelVal;

    AMFValue codeVal;
    codeVal.type = AMFValue::Type::String;
    codeVal.data = code;
    errorObj["code"] = codeVal;

    AMFValue descVal;
    descVal.type = AMFValue::Type::String;
    descVal.data = description;
    errorObj["description"] = descVal;

    AMFValue errorValue;
    errorValue.type = AMFValue::Type::Object;
    errorValue.data = errorObj;

    response.commandObject = AMFValue::makeNull();
    response.args.push_back(errorValue);

    return response;
}

CommandResponseMessage CommandHandler::createOnStatusResponse(
    const std::string& level,
    const std::string& code,
    const std::string& description,
    uint32_t messageStreamId
) const {
    CommandResponseMessage response;
    response.commandName = "onStatus";
    response.transactionId = 0.0;  // onStatus typically has 0 transaction ID
    response.statusCode = code;
    response.statusDescription = description;
    response.messageStreamId = messageStreamId;

    // Create info object
    std::map<std::string, AMFValue> infoObj;

    AMFValue levelVal;
    levelVal.type = AMFValue::Type::String;
    levelVal.data = level;
    infoObj["level"] = levelVal;

    AMFValue codeVal;
    codeVal.type = AMFValue::Type::String;
    codeVal.data = code;
    infoObj["code"] = codeVal;

    AMFValue descVal;
    descVal.type = AMFValue::Type::String;
    descVal.data = description;
    infoObj["description"] = descVal;

    AMFValue infoValue;
    infoValue.type = AMFValue::Type::Object;
    infoValue.data = infoObj;

    response.commandObject = AMFValue::makeNull();
    response.args.push_back(infoValue);

    return response;
}

std::string CommandHandler::extractAppName(const AMFValue& commandObject) const {
    if (!commandObject.isObject()) {
        return "";
    }

    const auto& obj = commandObject.asObject();
    auto it = obj.find("app");
    if (it != obj.end() && it->second.isString()) {
        return it->second.asString();
    }

    return "";
}

std::string CommandHandler::extractStreamName(const std::vector<AMFValue>& args) const {
    if (args.empty()) {
        return "";
    }

    if (args[0].isString()) {
        return args[0].asString();
    }

    return "";
}

StreamId CommandHandler::extractStreamIdArg(const std::vector<AMFValue>& args) const {
    if (args.empty()) {
        return 0;
    }

    if (args[0].isNumber()) {
        double val = args[0].asNumber();
        if (val > 0) {
            return static_cast<StreamId>(val);
        }
    }

    return 0;
}

bool CommandHandler::validateAppName(const std::string& appName) const {
    // Basic validation - app name must not be empty
    // Additional validation rules can be added as needed
    if (appName.empty()) {
        return false;
    }

    // Validate characters (alphanumeric, underscore, hyphen)
    for (char c : appName) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '/') {
            return false;
        }
    }

    return true;
}

} // namespace protocol
} // namespace openrtmp
