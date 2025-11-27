// OpenRTMP - Cross-platform RTMP Server
// Tests for Session State Machine
//
// Tests cover:
// - Track connection states: Connecting, Handshaking, Connected, Publishing, Subscribing, Disconnected
// - Enforce valid state transitions for RTMP commands
// - Associate sessions with connections and streams
// - Handle unexpected disconnection with state cleanup
// - Maintain session context for authentication and authorization
//
// Requirements coverage:
// - Requirement 3.3: publish command with stream key validation
// - Requirement 3.4: play command for subscription
// - Requirement 3.5: deleteStream for resource release
// - Requirement 3.6: closeStream for stream stop

#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

#include "openrtmp/streaming/session.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace streaming {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class SessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        session_ = std::make_unique<Session>(connectionId_);
    }

    void TearDown() override {
        session_.reset();
    }

    std::unique_ptr<Session> session_;
    ConnectionId connectionId_ = 100;
};

// =============================================================================
// Initial State Tests
// =============================================================================

TEST_F(SessionTest, InitialStateIsConnecting) {
    EXPECT_EQ(session_->state(), SessionState::Connecting);
}

TEST_F(SessionTest, InitialConnectionIdIsSet) {
    EXPECT_EQ(session_->connectionId(), connectionId_);
}

TEST_F(SessionTest, InitialStreamIdIsInvalid) {
    EXPECT_EQ(session_->streamId(), core::INVALID_STREAM_ID);
}

TEST_F(SessionTest, InitialStreamKeyIsEmpty) {
    EXPECT_TRUE(session_->streamKey().app.empty());
    EXPECT_TRUE(session_->streamKey().name.empty());
}

TEST_F(SessionTest, InitialAppNameIsEmpty) {
    EXPECT_TRUE(session_->appName().empty());
}

TEST_F(SessionTest, GeneratesUniqueSessionId) {
    auto session1 = std::make_unique<Session>(1);
    auto session2 = std::make_unique<Session>(2);
    auto session3 = std::make_unique<Session>(3);

    EXPECT_NE(session1->sessionId(), session2->sessionId());
    EXPECT_NE(session2->sessionId(), session3->sessionId());
    EXPECT_NE(session1->sessionId(), session3->sessionId());
    EXPECT_NE(session1->sessionId(), core::INVALID_SESSION_ID);
}

// =============================================================================
// State Transition Tests - Connecting State
// =============================================================================

TEST_F(SessionTest, TransitionFromConnectingToHandshaking) {
    auto result = session_->startHandshake();

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(session_->state(), SessionState::Handshaking);
}

TEST_F(SessionTest, TransitionFromConnectingToDisconnectedOnError) {
    auto result = session_->disconnect();

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(session_->state(), SessionState::Disconnected);
}

TEST_F(SessionTest, CannotTransitionFromConnectingToConnected) {
    auto result = session_->completeHandshake();

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, SessionError::Code::InvalidTransition);
    EXPECT_EQ(session_->state(), SessionState::Connecting);
}

TEST_F(SessionTest, CannotTransitionFromConnectingToPublishing) {
    StreamKey key("live", "test_stream");
    auto result = session_->startPublishing(key, 1);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, SessionError::Code::InvalidTransition);
    EXPECT_EQ(session_->state(), SessionState::Connecting);
}

TEST_F(SessionTest, CannotTransitionFromConnectingToSubscribing) {
    StreamKey key("live", "test_stream");
    auto result = session_->startSubscribing(key, 1);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, SessionError::Code::InvalidTransition);
    EXPECT_EQ(session_->state(), SessionState::Connecting);
}

// =============================================================================
// State Transition Tests - Handshaking State
// =============================================================================

TEST_F(SessionTest, TransitionFromHandshakingToConnected) {
    session_->startHandshake();

    auto result = session_->completeHandshake();

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(session_->state(), SessionState::Connected);
}

TEST_F(SessionTest, TransitionFromHandshakingToDisconnectedOnError) {
    session_->startHandshake();

    auto result = session_->disconnect();

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(session_->state(), SessionState::Disconnected);
}

TEST_F(SessionTest, CannotTransitionFromHandshakingToConnecting) {
    session_->startHandshake();

    // Cannot go back to connecting
    auto result = session_->startHandshake();

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, SessionError::Code::InvalidTransition);
    EXPECT_EQ(session_->state(), SessionState::Handshaking);
}

TEST_F(SessionTest, CannotTransitionFromHandshakingToPublishing) {
    session_->startHandshake();

    StreamKey key("live", "test_stream");
    auto result = session_->startPublishing(key, 1);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, SessionError::Code::InvalidTransition);
    EXPECT_EQ(session_->state(), SessionState::Handshaking);
}

// =============================================================================
// State Transition Tests - Connected State
// =============================================================================

TEST_F(SessionTest, TransitionFromConnectedToPublishing) {
    session_->startHandshake();
    session_->completeHandshake();
    session_->setAppName("live");

    StreamKey key("live", "test_stream");
    auto result = session_->startPublishing(key, 1);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(session_->state(), SessionState::Publishing);
    EXPECT_EQ(session_->streamKey(), key);
    EXPECT_EQ(session_->streamId(), 1u);
}

TEST_F(SessionTest, TransitionFromConnectedToSubscribing) {
    session_->startHandshake();
    session_->completeHandshake();
    session_->setAppName("live");

    StreamKey key("live", "test_stream");
    auto result = session_->startSubscribing(key, 1);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(session_->state(), SessionState::Subscribing);
    EXPECT_EQ(session_->streamKey(), key);
    EXPECT_EQ(session_->streamId(), 1u);
}

TEST_F(SessionTest, TransitionFromConnectedToDisconnected) {
    session_->startHandshake();
    session_->completeHandshake();

    auto result = session_->disconnect();

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(session_->state(), SessionState::Disconnected);
}

TEST_F(SessionTest, CannotTransitionFromConnectedToHandshaking) {
    session_->startHandshake();
    session_->completeHandshake();

    auto result = session_->startHandshake();

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, SessionError::Code::InvalidTransition);
    EXPECT_EQ(session_->state(), SessionState::Connected);
}

// =============================================================================
// State Transition Tests - Publishing State
// =============================================================================

TEST_F(SessionTest, TransitionFromPublishingToConnected) {
    session_->startHandshake();
    session_->completeHandshake();
    session_->setAppName("live");

    StreamKey key("live", "test_stream");
    session_->startPublishing(key, 1);

    auto result = session_->stopPublishing();

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(session_->state(), SessionState::Connected);
    EXPECT_TRUE(session_->streamKey().app.empty());
    EXPECT_TRUE(session_->streamKey().name.empty());
}

TEST_F(SessionTest, TransitionFromPublishingToDisconnected) {
    session_->startHandshake();
    session_->completeHandshake();
    session_->setAppName("live");

    StreamKey key("live", "test_stream");
    session_->startPublishing(key, 1);

    auto result = session_->disconnect();

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(session_->state(), SessionState::Disconnected);
}

TEST_F(SessionTest, CannotTransitionFromPublishingToSubscribing) {
    session_->startHandshake();
    session_->completeHandshake();
    session_->setAppName("live");

    StreamKey key1("live", "stream1");
    StreamKey key2("live", "stream2");
    session_->startPublishing(key1, 1);

    auto result = session_->startSubscribing(key2, 2);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, SessionError::Code::InvalidTransition);
    EXPECT_EQ(session_->state(), SessionState::Publishing);
}

// =============================================================================
// State Transition Tests - Subscribing State
// =============================================================================

TEST_F(SessionTest, TransitionFromSubscribingToConnected) {
    session_->startHandshake();
    session_->completeHandshake();
    session_->setAppName("live");

    StreamKey key("live", "test_stream");
    session_->startSubscribing(key, 1);

    auto result = session_->stopSubscribing();

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(session_->state(), SessionState::Connected);
    EXPECT_TRUE(session_->streamKey().app.empty());
    EXPECT_TRUE(session_->streamKey().name.empty());
}

TEST_F(SessionTest, TransitionFromSubscribingToDisconnected) {
    session_->startHandshake();
    session_->completeHandshake();
    session_->setAppName("live");

    StreamKey key("live", "test_stream");
    session_->startSubscribing(key, 1);

    auto result = session_->disconnect();

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(session_->state(), SessionState::Disconnected);
}

TEST_F(SessionTest, CannotTransitionFromSubscribingToPublishing) {
    session_->startHandshake();
    session_->completeHandshake();
    session_->setAppName("live");

    StreamKey key1("live", "stream1");
    StreamKey key2("live", "stream2");
    session_->startSubscribing(key1, 1);

    auto result = session_->startPublishing(key2, 2);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, SessionError::Code::InvalidTransition);
    EXPECT_EQ(session_->state(), SessionState::Subscribing);
}

// =============================================================================
// State Transition Tests - Disconnected State (Terminal State)
// =============================================================================

TEST_F(SessionTest, CannotTransitionFromDisconnected) {
    session_->disconnect();

    // Cannot transition to any other state
    EXPECT_TRUE(session_->startHandshake().isError());
    EXPECT_TRUE(session_->completeHandshake().isError());
    EXPECT_TRUE(session_->startPublishing(StreamKey("live", "test"), 1).isError());
    EXPECT_TRUE(session_->startSubscribing(StreamKey("live", "test"), 1).isError());
    EXPECT_EQ(session_->state(), SessionState::Disconnected);
}

TEST_F(SessionTest, DisconnectIsIdempotent) {
    session_->disconnect();
    auto result = session_->disconnect();

    // Disconnecting when already disconnected is a no-op
    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(session_->state(), SessionState::Disconnected);
}

// =============================================================================
// App Name Tests
// =============================================================================

TEST_F(SessionTest, SetAppNameSucceeds) {
    session_->startHandshake();
    session_->completeHandshake();

    session_->setAppName("live");

    EXPECT_EQ(session_->appName(), "live");
}

TEST_F(SessionTest, SetAppNameWithSlash) {
    session_->startHandshake();
    session_->completeHandshake();

    session_->setAppName("live/test");

    EXPECT_EQ(session_->appName(), "live/test");
}

// =============================================================================
// Stream ID Tests
// =============================================================================

TEST_F(SessionTest, AllocateStreamIdSetsStreamId) {
    session_->startHandshake();
    session_->completeHandshake();

    session_->setAllocatedStreamId(42);

    EXPECT_EQ(session_->allocatedStreamId(), 42u);
}

TEST_F(SessionTest, AllocatedStreamIdPersistsAcrossTransitions) {
    session_->startHandshake();
    session_->completeHandshake();
    session_->setAllocatedStreamId(42);
    session_->setAppName("live");

    StreamKey key("live", "test_stream");
    session_->startPublishing(key, 42);

    EXPECT_EQ(session_->allocatedStreamId(), 42u);

    session_->stopPublishing();

    // Allocated stream ID persists even after stopping publishing
    EXPECT_EQ(session_->allocatedStreamId(), 42u);
}

TEST_F(SessionTest, ReleaseStreamIdClearsAllocatedId) {
    session_->startHandshake();
    session_->completeHandshake();
    session_->setAllocatedStreamId(42);

    session_->releaseAllocatedStreamId();

    EXPECT_EQ(session_->allocatedStreamId(), core::INVALID_STREAM_ID);
}

// =============================================================================
// Authentication Context Tests
// =============================================================================

TEST_F(SessionTest, SetAuthenticatedFlag) {
    session_->startHandshake();
    session_->completeHandshake();

    EXPECT_FALSE(session_->isAuthenticated());

    session_->setAuthenticated(true);

    EXPECT_TRUE(session_->isAuthenticated());
}

TEST_F(SessionTest, AuthenticationContextPersistsAcrossPublishCycle) {
    session_->startHandshake();
    session_->completeHandshake();
    session_->setAppName("live");
    session_->setAuthenticated(true);

    StreamKey key("live", "test_stream");
    session_->startPublishing(key, 1);
    session_->stopPublishing();

    // Authentication persists
    EXPECT_TRUE(session_->isAuthenticated());
}

// =============================================================================
// Client Info Tests
// =============================================================================

TEST_F(SessionTest, SetClientInfo) {
    ClientInfo info;
    info.ip = "192.168.1.100";
    info.port = 12345;
    info.userAgent = "TestClient/1.0";

    session_->setClientInfo(info);

    const auto& storedInfo = session_->clientInfo();
    EXPECT_EQ(storedInfo.ip, "192.168.1.100");
    EXPECT_EQ(storedInfo.port, 12345);
    EXPECT_EQ(storedInfo.userAgent, "TestClient/1.0");
}

// =============================================================================
// Cleanup on Disconnect Tests
// =============================================================================

TEST_F(SessionTest, DisconnectClearsStreamKeyAndId) {
    session_->startHandshake();
    session_->completeHandshake();
    session_->setAppName("live");

    StreamKey key("live", "test_stream");
    session_->startPublishing(key, 1);

    EXPECT_EQ(session_->streamKey(), key);
    EXPECT_EQ(session_->streamId(), 1u);

    session_->disconnect();

    // Stream key and ID should be cleared on disconnect
    EXPECT_TRUE(session_->streamKey().app.empty());
    EXPECT_TRUE(session_->streamKey().name.empty());
    EXPECT_EQ(session_->streamId(), core::INVALID_STREAM_ID);
}

TEST_F(SessionTest, DisconnectPreservesAppNameAndConnectionId) {
    session_->startHandshake();
    session_->completeHandshake();
    session_->setAppName("live");

    session_->disconnect();

    // App name and connection ID are preserved for logging purposes
    EXPECT_EQ(session_->appName(), "live");
    EXPECT_EQ(session_->connectionId(), connectionId_);
}

// =============================================================================
// State Query Helper Tests
// =============================================================================

TEST_F(SessionTest, IsConnectedReturnsTrueInCorrectStates) {
    EXPECT_FALSE(session_->isConnected());

    session_->startHandshake();
    EXPECT_FALSE(session_->isConnected());

    session_->completeHandshake();
    EXPECT_TRUE(session_->isConnected());

    session_->setAppName("live");
    StreamKey key("live", "test");
    session_->startPublishing(key, 1);
    EXPECT_TRUE(session_->isConnected());

    session_->disconnect();
    EXPECT_FALSE(session_->isConnected());
}

TEST_F(SessionTest, IsPublishingReturnsCorrectly) {
    EXPECT_FALSE(session_->isPublishing());

    session_->startHandshake();
    session_->completeHandshake();
    session_->setAppName("live");

    EXPECT_FALSE(session_->isPublishing());

    StreamKey key("live", "test");
    session_->startPublishing(key, 1);
    EXPECT_TRUE(session_->isPublishing());

    session_->stopPublishing();
    EXPECT_FALSE(session_->isPublishing());
}

TEST_F(SessionTest, IsSubscribingReturnsCorrectly) {
    EXPECT_FALSE(session_->isSubscribing());

    session_->startHandshake();
    session_->completeHandshake();
    session_->setAppName("live");

    EXPECT_FALSE(session_->isSubscribing());

    StreamKey key("live", "test");
    session_->startSubscribing(key, 1);
    EXPECT_TRUE(session_->isSubscribing());

    session_->stopSubscribing();
    EXPECT_FALSE(session_->isSubscribing());
}

TEST_F(SessionTest, HasActiveStreamReturnsCorrectly) {
    EXPECT_FALSE(session_->hasActiveStream());

    session_->startHandshake();
    session_->completeHandshake();
    session_->setAppName("live");

    EXPECT_FALSE(session_->hasActiveStream());

    StreamKey key("live", "test");
    session_->startPublishing(key, 1);
    EXPECT_TRUE(session_->hasActiveStream());

    session_->stopPublishing();
    EXPECT_FALSE(session_->hasActiveStream());

    session_->startSubscribing(key, 2);
    EXPECT_TRUE(session_->hasActiveStream());
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

class SessionThreadSafetyTest : public ::testing::Test {
protected:
    void SetUp() override {
        session_ = std::make_unique<Session>(100);
    }

    std::unique_ptr<Session> session_;
};

TEST_F(SessionThreadSafetyTest, ConcurrentStateReadsAreSafe) {
    session_->startHandshake();
    session_->completeHandshake();

    const int numThreads = 10;
    const int readsPerThread = 1000;
    std::vector<std::thread> threads;
    std::atomic<int> readCount{0};

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, &readCount, readsPerThread]() {
            for (int j = 0; j < readsPerThread; ++j) {
                [[maybe_unused]] auto state = session_->state();
                [[maybe_unused]] auto connId = session_->connectionId();
                [[maybe_unused]] auto streamId = session_->streamId();
                [[maybe_unused]] auto key = session_->streamKey();
                readCount++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(readCount.load(), numThreads * readsPerThread);
    EXPECT_EQ(session_->state(), SessionState::Connected);
}

TEST_F(SessionThreadSafetyTest, ConcurrentTransitionAttempts) {
    session_->startHandshake();
    session_->completeHandshake();
    session_->setAppName("live");

    const int numThreads = 10;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    // Multiple threads try to transition to publishing at the same time
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, i, &successCount]() {
            StreamKey key("live", "stream_" + std::to_string(i));
            auto result = session_->startPublishing(key, static_cast<StreamId>(i + 1));
            if (result.isSuccess()) {
                successCount++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Only one thread should succeed
    EXPECT_EQ(successCount.load(), 1);
    EXPECT_EQ(session_->state(), SessionState::Publishing);
}

TEST_F(SessionThreadSafetyTest, ConcurrentDisconnectAttempts) {
    session_->startHandshake();
    session_->completeHandshake();

    const int numThreads = 10;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, &successCount]() {
            auto result = session_->disconnect();
            if (result.isSuccess()) {
                successCount++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All disconnect calls should succeed (idempotent)
    EXPECT_EQ(successCount.load(), numThreads);
    EXPECT_EQ(session_->state(), SessionState::Disconnected);
}

// =============================================================================
// Event Callback Tests
// =============================================================================

TEST_F(SessionTest, StateChangeCallbackIsInvokedOnTransition) {
    bool callbackInvoked = false;
    SessionState oldState = SessionState::Disconnected;
    SessionState newState = SessionState::Disconnected;

    session_->setStateChangeCallback([&](SessionState from, SessionState to) {
        callbackInvoked = true;
        oldState = from;
        newState = to;
    });

    session_->startHandshake();

    EXPECT_TRUE(callbackInvoked);
    EXPECT_EQ(oldState, SessionState::Connecting);
    EXPECT_EQ(newState, SessionState::Handshaking);
}

TEST_F(SessionTest, StateChangeCallbackNotInvokedOnFailedTransition) {
    bool callbackInvoked = false;

    session_->setStateChangeCallback([&](SessionState, SessionState) {
        callbackInvoked = true;
    });

    // Try invalid transition from Connecting to Connected
    session_->completeHandshake();

    EXPECT_FALSE(callbackInvoked);
}

// =============================================================================
// Timestamp Tests
// =============================================================================

TEST_F(SessionTest, CreatedAtTimestampIsSet) {
    auto now = std::chrono::steady_clock::now();
    auto createdAt = session_->createdAt();

    // Should be within 1 second of now
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - createdAt).count();
    EXPECT_GE(diff, 0);
    EXPECT_LE(diff, 1);
}

TEST_F(SessionTest, LastActivityTimestampUpdatesOnStateChange) {
    auto initial = session_->lastActivity();

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    session_->startHandshake();

    auto afterTransition = session_->lastActivity();

    EXPECT_GT(afterTransition, initial);
}

} // namespace test
} // namespace streaming
} // namespace openrtmp
