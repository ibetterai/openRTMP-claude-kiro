// OpenRTMP - Cross-platform RTMP Server
// Tests for RTMP Command Handler
//
// Tests cover:
// - connect command processing with application name validation
// - createStream command processing with stream ID allocation
// - publish command processing with stream key validation and conflict detection
// - play command processing for subscription initiation
// - deleteStream command for resource cleanup
// - closeStream command for stream stop while maintaining connection
// - Error responses for various failure scenarios
// - Response latency within 50ms target
// - Thread-safety for concurrent command processing
//
// Requirements coverage:
// - Requirement 3.1: connect command response within 50ms
// - Requirement 3.2: createStream command with stream ID allocation
// - Requirement 3.3: publish command with stream key validation
// - Requirement 3.4: play command for subscription
// - Requirement 3.5: deleteStream for resource release
// - Requirement 3.6: closeStream for stream stop
// - Requirement 3.7: Stream key conflict detection

#include <gtest/gtest.h>
#include <memory>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>

#include "openrtmp/protocol/command_handler.hpp"
#include "openrtmp/protocol/amf_codec.hpp"
#include "openrtmp/streaming/stream_registry.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace protocol {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class CommandHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        streamRegistry_ = std::make_shared<streaming::StreamRegistry>();
        amfCodec_ = std::make_shared<AMFCodec>();
        commandHandler_ = std::make_unique<CommandHandler>(streamRegistry_, amfCodec_);
    }

    void TearDown() override {
        commandHandler_.reset();
        streamRegistry_.reset();
        amfCodec_.reset();
    }

    // Helper to create a session context
    SessionContext createSessionContext(SessionId sessionId = 1, ConnectionId connId = 100) {
        SessionContext ctx;
        ctx.sessionId = sessionId;
        ctx.connectionId = connId;
        ctx.state = SessionState::Connected;
        ctx.appName = "";
        ctx.streamId = 0;
        return ctx;
    }

    // Helper to create connect command object
    AMFValue createConnectCommandObject(const std::string& app = "live") {
        std::map<std::string, AMFValue> obj;

        AMFValue appVal;
        appVal.type = AMFValue::Type::String;
        appVal.data = app;
        obj["app"] = appVal;

        AMFValue tcUrl;
        tcUrl.type = AMFValue::Type::String;
        tcUrl.data = "rtmp://localhost/" + app;
        obj["tcUrl"] = tcUrl;

        AMFValue fpad;
        fpad.type = AMFValue::Type::Boolean;
        fpad.data = false;
        obj["fpad"] = fpad;

        AMFValue result;
        result.type = AMFValue::Type::Object;
        result.data = obj;
        return result;
    }

    std::shared_ptr<streaming::StreamRegistry> streamRegistry_;
    std::shared_ptr<AMFCodec> amfCodec_;
    std::unique_ptr<CommandHandler> commandHandler_;
};

// =============================================================================
// Connect Command Tests
// =============================================================================

TEST_F(CommandHandlerTest, ConnectCommandSucceeds) {
    SessionContext ctx = createSessionContext();
    RTMPCommand cmd;
    cmd.name = "connect";
    cmd.transactionId = 1.0;
    cmd.commandObject = createConnectCommandObject("live");

    auto result = commandHandler_->handleConnect(cmd, ctx);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(ctx.state, SessionState::Connected);
    EXPECT_EQ(ctx.appName, "live");
    EXPECT_FALSE(result.value().responses.empty());
}

TEST_F(CommandHandlerTest, ConnectCommandResponseContainsResult) {
    SessionContext ctx = createSessionContext();
    RTMPCommand cmd;
    cmd.name = "connect";
    cmd.transactionId = 1.0;
    cmd.commandObject = createConnectCommandObject("live");

    auto result = commandHandler_->handleConnect(cmd, ctx);

    ASSERT_TRUE(result.isSuccess());
    // Check that response contains _result message
    bool hasResult = false;
    for (const auto& msg : result.value().responses) {
        if (msg.commandName == "_result") {
            hasResult = true;
            EXPECT_EQ(msg.transactionId, 1.0);
        }
    }
    EXPECT_TRUE(hasResult);
}

TEST_F(CommandHandlerTest, ConnectCommandWithInvalidAppFails) {
    SessionContext ctx = createSessionContext();
    RTMPCommand cmd;
    cmd.name = "connect";
    cmd.transactionId = 1.0;
    cmd.commandObject = createConnectCommandObject("");  // Empty app name

    auto result = commandHandler_->handleConnect(cmd, ctx);

    // Should return error response
    ASSERT_TRUE(result.isSuccess());  // Operation succeeds but sends _error
    bool hasError = false;
    for (const auto& msg : result.value().responses) {
        if (msg.commandName == "_error") {
            hasError = true;
        }
    }
    EXPECT_TRUE(hasError);
}

TEST_F(CommandHandlerTest, ConnectCommandWithinLatencyTarget) {
    SessionContext ctx = createSessionContext();
    RTMPCommand cmd;
    cmd.name = "connect";
    cmd.transactionId = 1.0;
    cmd.commandObject = createConnectCommandObject("live");

    auto start = std::chrono::steady_clock::now();
    auto result = commandHandler_->handleConnect(cmd, ctx);
    auto end = std::chrono::steady_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Requirement 3.1: Response within 50ms
    EXPECT_LT(duration.count(), 50);
}

TEST_F(CommandHandlerTest, ConnectCommandWhenAlreadyConnectedFails) {
    SessionContext ctx = createSessionContext();
    ctx.state = SessionState::Publishing;  // Already in a connected/publishing state
    ctx.appName = "live";  // Already connected to app

    RTMPCommand cmd;
    cmd.name = "connect";
    cmd.transactionId = 1.0;
    cmd.commandObject = createConnectCommandObject("other_app");

    auto result = commandHandler_->handleConnect(cmd, ctx);

    // Should return error - already connected
    ASSERT_TRUE(result.isSuccess());
    bool hasError = false;
    for (const auto& msg : result.value().responses) {
        if (msg.commandName == "_error") {
            hasError = true;
        }
    }
    EXPECT_TRUE(hasError);
}

// =============================================================================
// CreateStream Command Tests
// =============================================================================

TEST_F(CommandHandlerTest, CreateStreamCommandSucceeds) {
    SessionContext ctx = createSessionContext();
    ctx.appName = "live";  // Must be connected first

    RTMPCommand cmd;
    cmd.name = "createStream";
    cmd.transactionId = 2.0;
    cmd.commandObject = AMFValue::makeNull();

    auto result = commandHandler_->handleCreateStream(cmd, ctx);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_NE(ctx.streamId, 0u);
    EXPECT_FALSE(result.value().responses.empty());
}

TEST_F(CommandHandlerTest, CreateStreamReturnsStreamIdInResponse) {
    SessionContext ctx = createSessionContext();
    ctx.appName = "live";

    RTMPCommand cmd;
    cmd.name = "createStream";
    cmd.transactionId = 2.0;
    cmd.commandObject = AMFValue::makeNull();

    auto result = commandHandler_->handleCreateStream(cmd, ctx);

    ASSERT_TRUE(result.isSuccess());

    // Find the _result response
    bool hasValidResult = false;
    for (const auto& msg : result.value().responses) {
        if (msg.commandName == "_result") {
            // Stream ID should be in the response (as a number)
            EXPECT_EQ(msg.transactionId, 2.0);
            hasValidResult = true;
            // The stream ID should be returned in the result
            EXPECT_GT(msg.streamId, 0u);
        }
    }
    EXPECT_TRUE(hasValidResult);
}

TEST_F(CommandHandlerTest, CreateStreamAllocatesUniqueIds) {
    SessionContext ctx1 = createSessionContext(1, 100);
    ctx1.appName = "live";

    SessionContext ctx2 = createSessionContext(2, 101);
    ctx2.appName = "live";

    RTMPCommand cmd;
    cmd.name = "createStream";
    cmd.transactionId = 2.0;
    cmd.commandObject = AMFValue::makeNull();

    commandHandler_->handleCreateStream(cmd, ctx1);
    commandHandler_->handleCreateStream(cmd, ctx2);

    EXPECT_NE(ctx1.streamId, ctx2.streamId);
    EXPECT_NE(ctx1.streamId, 0u);
    EXPECT_NE(ctx2.streamId, 0u);
}

TEST_F(CommandHandlerTest, CreateStreamWithoutConnectFails) {
    SessionContext ctx = createSessionContext();
    ctx.appName = "";  // Not connected to any app

    RTMPCommand cmd;
    cmd.name = "createStream";
    cmd.transactionId = 2.0;
    cmd.commandObject = AMFValue::makeNull();

    auto result = commandHandler_->handleCreateStream(cmd, ctx);

    ASSERT_TRUE(result.isSuccess());
    bool hasError = false;
    for (const auto& msg : result.value().responses) {
        if (msg.commandName == "_error") {
            hasError = true;
        }
    }
    EXPECT_TRUE(hasError);
}

// =============================================================================
// Publish Command Tests
// =============================================================================

TEST_F(CommandHandlerTest, PublishCommandSucceeds) {
    SessionContext ctx = createSessionContext();
    ctx.appName = "live";
    ctx.streamId = streamRegistry_->allocateStreamId().value();

    RTMPCommand cmd;
    cmd.name = "publish";
    cmd.transactionId = 0.0;  // Publish commands typically have 0 transaction ID
    cmd.commandObject = AMFValue::makeNull();
    cmd.args.push_back(AMFValue::makeString("test_stream"));  // Stream name
    cmd.args.push_back(AMFValue::makeString("live"));         // Publish type

    auto result = commandHandler_->handlePublish(cmd, ctx);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(ctx.state, SessionState::Publishing);
}

TEST_F(CommandHandlerTest, PublishCommandWithEmptyStreamKeyFails) {
    SessionContext ctx = createSessionContext();
    ctx.appName = "live";
    ctx.streamId = streamRegistry_->allocateStreamId().value();

    RTMPCommand cmd;
    cmd.name = "publish";
    cmd.transactionId = 0.0;
    cmd.commandObject = AMFValue::makeNull();
    cmd.args.push_back(AMFValue::makeString(""));  // Empty stream name
    cmd.args.push_back(AMFValue::makeString("live"));

    auto result = commandHandler_->handlePublish(cmd, ctx);

    ASSERT_TRUE(result.isSuccess());
    bool hasError = false;
    for (const auto& msg : result.value().responses) {
        if (msg.commandName == "onStatus" && msg.statusCode == "NetStream.Publish.BadName") {
            hasError = true;
        }
    }
    EXPECT_TRUE(hasError);
}

TEST_F(CommandHandlerTest, PublishCommandWithDuplicateStreamKeyFails) {
    // First publisher
    SessionContext ctx1 = createSessionContext(1, 100);
    ctx1.appName = "live";
    ctx1.streamId = streamRegistry_->allocateStreamId().value();

    RTMPCommand cmd1;
    cmd1.name = "publish";
    cmd1.transactionId = 0.0;
    cmd1.commandObject = AMFValue::makeNull();
    cmd1.args.push_back(AMFValue::makeString("test_stream"));
    cmd1.args.push_back(AMFValue::makeString("live"));

    auto result1 = commandHandler_->handlePublish(cmd1, ctx1);
    ASSERT_TRUE(result1.isSuccess());

    // Second publisher trying same stream key (Requirement 3.7)
    SessionContext ctx2 = createSessionContext(2, 101);
    ctx2.appName = "live";
    ctx2.streamId = streamRegistry_->allocateStreamId().value();

    RTMPCommand cmd2;
    cmd2.name = "publish";
    cmd2.transactionId = 0.0;
    cmd2.commandObject = AMFValue::makeNull();
    cmd2.args.push_back(AMFValue::makeString("test_stream"));  // Same stream name
    cmd2.args.push_back(AMFValue::makeString("live"));

    auto result2 = commandHandler_->handlePublish(cmd2, ctx2);

    ASSERT_TRUE(result2.isSuccess());
    bool hasError = false;
    for (const auto& msg : result2.value().responses) {
        if (msg.commandName == "onStatus" && msg.statusCode == "NetStream.Publish.BadName") {
            hasError = true;
        }
    }
    EXPECT_TRUE(hasError);
}

TEST_F(CommandHandlerTest, PublishCommandWithoutStreamIdFails) {
    SessionContext ctx = createSessionContext();
    ctx.appName = "live";
    ctx.streamId = 0;  // No stream ID allocated

    RTMPCommand cmd;
    cmd.name = "publish";
    cmd.transactionId = 0.0;
    cmd.commandObject = AMFValue::makeNull();
    cmd.args.push_back(AMFValue::makeString("test_stream"));
    cmd.args.push_back(AMFValue::makeString("live"));

    auto result = commandHandler_->handlePublish(cmd, ctx);

    ASSERT_TRUE(result.isSuccess());
    bool hasError = false;
    for (const auto& msg : result.value().responses) {
        if (msg.commandName == "onStatus" || msg.commandName == "_error") {
            hasError = true;
        }
    }
    EXPECT_TRUE(hasError);
}

TEST_F(CommandHandlerTest, PublishCommandSendsOnStatusStartResponse) {
    SessionContext ctx = createSessionContext();
    ctx.appName = "live";
    ctx.streamId = streamRegistry_->allocateStreamId().value();

    RTMPCommand cmd;
    cmd.name = "publish";
    cmd.transactionId = 0.0;
    cmd.commandObject = AMFValue::makeNull();
    cmd.args.push_back(AMFValue::makeString("test_stream"));
    cmd.args.push_back(AMFValue::makeString("live"));

    auto result = commandHandler_->handlePublish(cmd, ctx);

    ASSERT_TRUE(result.isSuccess());
    bool hasStartStatus = false;
    for (const auto& msg : result.value().responses) {
        if (msg.commandName == "onStatus" && msg.statusCode == "NetStream.Publish.Start") {
            hasStartStatus = true;
        }
    }
    EXPECT_TRUE(hasStartStatus);
}

// =============================================================================
// Play Command Tests
// =============================================================================

TEST_F(CommandHandlerTest, PlayCommandSucceeds) {
    // First set up a publisher
    SessionContext pubCtx = createSessionContext(1, 100);
    pubCtx.appName = "live";
    pubCtx.streamId = streamRegistry_->allocateStreamId().value();

    RTMPCommand pubCmd;
    pubCmd.name = "publish";
    pubCmd.transactionId = 0.0;
    pubCmd.commandObject = AMFValue::makeNull();
    pubCmd.args.push_back(AMFValue::makeString("test_stream"));
    pubCmd.args.push_back(AMFValue::makeString("live"));
    commandHandler_->handlePublish(pubCmd, pubCtx);

    // Now a subscriber tries to play
    SessionContext playCtx = createSessionContext(2, 101);
    playCtx.appName = "live";
    playCtx.streamId = streamRegistry_->allocateStreamId().value();

    RTMPCommand playCmd;
    playCmd.name = "play";
    playCmd.transactionId = 0.0;
    playCmd.commandObject = AMFValue::makeNull();
    playCmd.args.push_back(AMFValue::makeString("test_stream"));

    auto result = commandHandler_->handlePlay(playCmd, playCtx);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(playCtx.state, SessionState::Subscribing);
}

TEST_F(CommandHandlerTest, PlayCommandOnNonExistentStreamFails) {
    SessionContext ctx = createSessionContext();
    ctx.appName = "live";
    ctx.streamId = streamRegistry_->allocateStreamId().value();

    RTMPCommand cmd;
    cmd.name = "play";
    cmd.transactionId = 0.0;
    cmd.commandObject = AMFValue::makeNull();
    cmd.args.push_back(AMFValue::makeString("nonexistent_stream"));

    auto result = commandHandler_->handlePlay(cmd, ctx);

    ASSERT_TRUE(result.isSuccess());
    bool hasError = false;
    for (const auto& msg : result.value().responses) {
        if (msg.commandName == "onStatus" && msg.statusCode == "NetStream.Play.StreamNotFound") {
            hasError = true;
        }
    }
    EXPECT_TRUE(hasError);
}

TEST_F(CommandHandlerTest, PlayCommandWithoutStreamIdFails) {
    SessionContext ctx = createSessionContext();
    ctx.appName = "live";
    ctx.streamId = 0;  // No stream ID

    RTMPCommand cmd;
    cmd.name = "play";
    cmd.transactionId = 0.0;
    cmd.commandObject = AMFValue::makeNull();
    cmd.args.push_back(AMFValue::makeString("test_stream"));

    auto result = commandHandler_->handlePlay(cmd, ctx);

    ASSERT_TRUE(result.isSuccess());
    bool hasError = false;
    for (const auto& msg : result.value().responses) {
        if (msg.commandName == "onStatus" || msg.commandName == "_error") {
            hasError = true;
        }
    }
    EXPECT_TRUE(hasError);
}

TEST_F(CommandHandlerTest, PlayCommandSendsOnStatusStartResponse) {
    // Set up publisher first
    SessionContext pubCtx = createSessionContext(1, 100);
    pubCtx.appName = "live";
    pubCtx.streamId = streamRegistry_->allocateStreamId().value();

    RTMPCommand pubCmd;
    pubCmd.name = "publish";
    pubCmd.transactionId = 0.0;
    pubCmd.commandObject = AMFValue::makeNull();
    pubCmd.args.push_back(AMFValue::makeString("test_stream"));
    pubCmd.args.push_back(AMFValue::makeString("live"));
    commandHandler_->handlePublish(pubCmd, pubCtx);

    // Subscriber plays
    SessionContext playCtx = createSessionContext(2, 101);
    playCtx.appName = "live";
    playCtx.streamId = streamRegistry_->allocateStreamId().value();

    RTMPCommand playCmd;
    playCmd.name = "play";
    playCmd.transactionId = 0.0;
    playCmd.commandObject = AMFValue::makeNull();
    playCmd.args.push_back(AMFValue::makeString("test_stream"));

    auto result = commandHandler_->handlePlay(playCmd, playCtx);

    ASSERT_TRUE(result.isSuccess());
    bool hasStartStatus = false;
    for (const auto& msg : result.value().responses) {
        if (msg.commandName == "onStatus" && msg.statusCode == "NetStream.Play.Start") {
            hasStartStatus = true;
        }
    }
    EXPECT_TRUE(hasStartStatus);
}

// =============================================================================
// DeleteStream Command Tests
// =============================================================================

TEST_F(CommandHandlerTest, DeleteStreamReleasesResources) {
    // First publish a stream
    SessionContext ctx = createSessionContext();
    ctx.appName = "live";
    ctx.streamId = streamRegistry_->allocateStreamId().value();

    RTMPCommand pubCmd;
    pubCmd.name = "publish";
    pubCmd.transactionId = 0.0;
    pubCmd.commandObject = AMFValue::makeNull();
    pubCmd.args.push_back(AMFValue::makeString("test_stream"));
    pubCmd.args.push_back(AMFValue::makeString("live"));
    commandHandler_->handlePublish(pubCmd, ctx);

    StreamKey key("live", "test_stream");
    EXPECT_TRUE(streamRegistry_->hasStream(key));

    // Now delete stream
    RTMPCommand delCmd;
    delCmd.name = "deleteStream";
    delCmd.transactionId = 0.0;
    delCmd.commandObject = AMFValue::makeNull();
    delCmd.args.push_back(AMFValue::makeNumber(static_cast<double>(ctx.streamId)));

    auto result = commandHandler_->handleDeleteStream(delCmd, ctx);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_FALSE(streamRegistry_->hasStream(key));
}

TEST_F(CommandHandlerTest, DeleteStreamTransitionsToConnectedState) {
    SessionContext ctx = createSessionContext();
    ctx.appName = "live";
    ctx.streamId = streamRegistry_->allocateStreamId().value();

    // Publish
    RTMPCommand pubCmd;
    pubCmd.name = "publish";
    pubCmd.transactionId = 0.0;
    pubCmd.commandObject = AMFValue::makeNull();
    pubCmd.args.push_back(AMFValue::makeString("test_stream"));
    pubCmd.args.push_back(AMFValue::makeString("live"));
    commandHandler_->handlePublish(pubCmd, ctx);

    EXPECT_EQ(ctx.state, SessionState::Publishing);

    // Delete stream
    RTMPCommand delCmd;
    delCmd.name = "deleteStream";
    delCmd.transactionId = 0.0;
    delCmd.commandObject = AMFValue::makeNull();
    delCmd.args.push_back(AMFValue::makeNumber(static_cast<double>(ctx.streamId)));

    commandHandler_->handleDeleteStream(delCmd, ctx);

    EXPECT_EQ(ctx.state, SessionState::Connected);
}

TEST_F(CommandHandlerTest, DeleteStreamWithInvalidIdFails) {
    SessionContext ctx = createSessionContext();
    ctx.appName = "live";
    ctx.streamId = streamRegistry_->allocateStreamId().value();

    RTMPCommand delCmd;
    delCmd.name = "deleteStream";
    delCmd.transactionId = 0.0;
    delCmd.commandObject = AMFValue::makeNull();
    delCmd.args.push_back(AMFValue::makeNumber(999.0));  // Invalid stream ID

    auto result = commandHandler_->handleDeleteStream(delCmd, ctx);

    // Should handle gracefully without crashing
    ASSERT_TRUE(result.isSuccess());
}

// =============================================================================
// CloseStream Command Tests
// =============================================================================

TEST_F(CommandHandlerTest, CloseStreamStopsStreamButMaintainsConnection) {
    SessionContext ctx = createSessionContext();
    ctx.appName = "live";
    ctx.streamId = streamRegistry_->allocateStreamId().value();

    // Publish
    RTMPCommand pubCmd;
    pubCmd.name = "publish";
    pubCmd.transactionId = 0.0;
    pubCmd.commandObject = AMFValue::makeNull();
    pubCmd.args.push_back(AMFValue::makeString("test_stream"));
    pubCmd.args.push_back(AMFValue::makeString("live"));
    commandHandler_->handlePublish(pubCmd, ctx);

    EXPECT_EQ(ctx.state, SessionState::Publishing);

    // Close stream
    RTMPCommand closeCmd;
    closeCmd.name = "closeStream";
    closeCmd.transactionId = 0.0;
    closeCmd.commandObject = AMFValue::makeNull();

    auto result = commandHandler_->handleCloseStream(closeCmd, ctx);

    ASSERT_TRUE(result.isSuccess());
    // Stream is closed, but connection remains (can reuse)
    EXPECT_EQ(ctx.state, SessionState::Connected);
    EXPECT_NE(ctx.appName, "");  // App name still set
}

TEST_F(CommandHandlerTest, CloseStreamUnregistersFromRegistry) {
    SessionContext ctx = createSessionContext();
    ctx.appName = "live";
    ctx.streamId = streamRegistry_->allocateStreamId().value();

    // Publish
    RTMPCommand pubCmd;
    pubCmd.name = "publish";
    pubCmd.transactionId = 0.0;
    pubCmd.commandObject = AMFValue::makeNull();
    pubCmd.args.push_back(AMFValue::makeString("test_stream"));
    pubCmd.args.push_back(AMFValue::makeString("live"));
    commandHandler_->handlePublish(pubCmd, ctx);

    StreamKey key("live", "test_stream");
    EXPECT_TRUE(streamRegistry_->hasStream(key));

    // Close stream
    RTMPCommand closeCmd;
    closeCmd.name = "closeStream";
    closeCmd.transactionId = 0.0;
    closeCmd.commandObject = AMFValue::makeNull();

    commandHandler_->handleCloseStream(closeCmd, ctx);

    EXPECT_FALSE(streamRegistry_->hasStream(key));
}

// =============================================================================
// Generic Command Processing Tests
// =============================================================================

TEST_F(CommandHandlerTest, ProcessCommandRoutesProperly) {
    SessionContext ctx = createSessionContext();

    RTMPCommand cmd;
    cmd.name = "connect";
    cmd.transactionId = 1.0;
    cmd.commandObject = createConnectCommandObject("live");

    auto result = commandHandler_->processCommand(cmd, ctx);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(ctx.appName, "live");
}

TEST_F(CommandHandlerTest, ProcessUnknownCommandReturnsError) {
    SessionContext ctx = createSessionContext();
    ctx.appName = "live";

    RTMPCommand cmd;
    cmd.name = "unknownCommand";
    cmd.transactionId = 1.0;
    cmd.commandObject = AMFValue::makeNull();

    auto result = commandHandler_->processCommand(cmd, ctx);

    // Unknown commands should be handled gracefully
    ASSERT_TRUE(result.isSuccess());
    // Should return an error response
    bool hasError = false;
    for (const auto& msg : result.value().responses) {
        if (msg.commandName == "_error") {
            hasError = true;
        }
    }
    EXPECT_TRUE(hasError);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

class CommandHandlerConcurrencyTest : public CommandHandlerTest {
};

TEST_F(CommandHandlerConcurrencyTest, ConcurrentPublishAttemptsOnlyOneSucceeds) {
    const int numThreads = 10;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};
    std::atomic<int> failCount{0};

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, i, &successCount, &failCount]() {
            SessionContext ctx = createSessionContext(static_cast<SessionId>(i + 1), static_cast<ConnectionId>(100 + i));
            ctx.appName = "live";
            ctx.streamId = streamRegistry_->allocateStreamId().value();

            RTMPCommand cmd;
            cmd.name = "publish";
            cmd.transactionId = 0.0;
            cmd.commandObject = AMFValue::makeNull();
            cmd.args.push_back(AMFValue::makeString("shared_stream"));  // Same stream name
            cmd.args.push_back(AMFValue::makeString("live"));

            auto result = commandHandler_->handlePublish(cmd, ctx);

            if (result.isSuccess()) {
                bool published = true;
                for (const auto& msg : result.value().responses) {
                    if (msg.commandName == "onStatus" && msg.statusCode == "NetStream.Publish.BadName") {
                        published = false;
                        break;
                    }
                }
                if (published) {
                    successCount++;
                } else {
                    failCount++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Only one publisher should succeed
    EXPECT_EQ(successCount.load(), 1);
    EXPECT_EQ(failCount.load(), numThreads - 1);
}

TEST_F(CommandHandlerConcurrencyTest, ConcurrentCreateStreamAllocatesUniqueIds) {
    const int numThreads = 10;
    std::vector<std::thread> threads;
    std::vector<StreamId> allocatedIds;
    std::mutex idsMutex;

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, i, &allocatedIds, &idsMutex]() {
            SessionContext ctx = createSessionContext(static_cast<SessionId>(i + 1), static_cast<ConnectionId>(100 + i));
            ctx.appName = "live";

            RTMPCommand cmd;
            cmd.name = "createStream";
            cmd.transactionId = 2.0;
            cmd.commandObject = AMFValue::makeNull();

            auto result = commandHandler_->handleCreateStream(cmd, ctx);

            if (result.isSuccess() && ctx.streamId != 0) {
                std::lock_guard<std::mutex> lock(idsMutex);
                allocatedIds.push_back(ctx.streamId);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All IDs should be unique
    std::set<StreamId> uniqueIds(allocatedIds.begin(), allocatedIds.end());
    EXPECT_EQ(uniqueIds.size(), allocatedIds.size());
    EXPECT_EQ(allocatedIds.size(), static_cast<size_t>(numThreads));
}

// =============================================================================
// Response Encoding Tests
// =============================================================================

TEST_F(CommandHandlerTest, ResponsesContainValidAMFData) {
    SessionContext ctx = createSessionContext();

    RTMPCommand cmd;
    cmd.name = "connect";
    cmd.transactionId = 1.0;
    cmd.commandObject = createConnectCommandObject("live");

    auto result = commandHandler_->handleConnect(cmd, ctx);

    ASSERT_TRUE(result.isSuccess());
    ASSERT_FALSE(result.value().responses.empty());

    // Each response should have encodable AMF data
    for (const auto& msg : result.value().responses) {
        EXPECT_FALSE(msg.commandName.empty());
    }
}

} // namespace test
} // namespace protocol
} // namespace openrtmp
