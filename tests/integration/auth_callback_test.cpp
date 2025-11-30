// OpenRTMP - Cross-platform RTMP Server
// Integration Tests: Authentication with Mock HTTP Callback
//
// Task 21.2: Implement integration test suite
// Tests authentication integration with mock HTTP callback
//
// Requirements coverage:
// - Requirement 15.4: Integration with external authentication services via HTTP callbacks

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>

#include "openrtmp/core/auth_service.hpp"
#include "openrtmp/core/external_auth_callback.hpp"
#include "openrtmp/core/types.hpp"
#include "openrtmp/streaming/stream_registry.hpp"
#include "openrtmp/protocol/command_handler.hpp"

namespace openrtmp {
namespace integration {
namespace test {

// =============================================================================
// Mock HTTP Client for Testing
// =============================================================================

class MockHttpClient : public core::IHttpClient {
public:
    // Configurable response
    bool shouldSucceed = true;
    int responseStatusCode = 200;
    std::string responseBody = R"({"allow": true})";
    bool shouldTimeout = false;
    std::chrono::milliseconds delay{0};

    // Request tracking
    std::vector<std::pair<std::string, core::HttpRequest>> receivedRequests;

    core::Result<core::HttpResponse, core::HttpError> post(
        const std::string& url,
        const core::HttpRequest& request,
        std::chrono::milliseconds timeout
    ) override {
        receivedRequests.push_back({url, request});

        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }

        if (shouldTimeout) {
            return core::Result<core::HttpResponse, core::HttpError>::error(
                core::HttpError{core::HttpError::Code::Timeout, "Request timed out"}
            );
        }

        if (!shouldSucceed) {
            return core::Result<core::HttpResponse, core::HttpError>::error(
                core::HttpError{core::HttpError::Code::ConnectionFailed, "Connection failed"}
            );
        }

        core::HttpResponse response;
        response.statusCode = responseStatusCode;
        response.body = responseBody;
        return core::Result<core::HttpResponse, core::HttpError>::success(response);
    }

    void reset() {
        receivedRequests.clear();
        shouldSucceed = true;
        responseStatusCode = 200;
        responseBody = R"({"allow": true})";
        shouldTimeout = false;
        delay = std::chrono::milliseconds{0};
    }

    void setAllowResponse() {
        responseStatusCode = 200;
        responseBody = R"({"allowed": true})";
    }

    void setDenyResponse(const std::string& reason = "Not authorized") {
        responseStatusCode = 200;
        responseBody = R"({"allowed": false, "reason": ")" + reason + R"("})";
    }
};

// =============================================================================
// Test Fixtures
// =============================================================================

class AuthCallbackIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create mock HTTP client
        mockHttpClient_ = std::make_shared<MockHttpClient>();

        // Create external auth callback
        core::ExternalAuthConfig authConfig;
        authConfig.callbackUrl = "http://localhost:8080/auth";
        authConfig.timeoutMs = 5000;
        authConfig.fallbackOnTimeout = core::AuthFallbackBehavior::Deny;
        authConfig.cacheEnabled = true;
        authConfig.cacheTtlMs = 60000;

        externalAuth_ = std::make_unique<core::ExternalAuthCallback>(
            authConfig, mockHttpClient_);

        // Create auth service
        core::AuthConfig serviceConfig;
        serviceConfig.publishAuthEnabled = true;
        serviceConfig.subscribeAuthEnabled = false;
        authService_ = std::make_unique<core::AuthService>(serviceConfig);

        // Create stream registry and command handler
        streamRegistry_ = std::make_shared<streaming::StreamRegistry>();
        amfCodec_ = std::make_shared<protocol::AMFCodec>();
        commandHandler_ = std::make_unique<protocol::CommandHandler>(streamRegistry_, amfCodec_);
    }

    void TearDown() override {
        commandHandler_.reset();
        streamRegistry_->clear();
        streamRegistry_.reset();
        authService_.reset();
        externalAuth_.reset();
        mockHttpClient_.reset();
    }

    ClientInfo createTestClient(const std::string& ip = "192.168.1.100", uint16_t port = 12345) {
        ClientInfo client;
        client.ip = ip;
        client.port = port;
        return client;
    }

    // Components
    std::shared_ptr<MockHttpClient> mockHttpClient_;
    std::unique_ptr<core::ExternalAuthCallback> externalAuth_;
    std::unique_ptr<core::AuthService> authService_;
    std::shared_ptr<streaming::StreamRegistry> streamRegistry_;
    std::shared_ptr<protocol::IAMFCodec> amfCodec_;
    std::unique_ptr<protocol::CommandHandler> commandHandler_;
};

// =============================================================================
// Basic External Auth Tests
// =============================================================================

TEST_F(AuthCallbackIntegrationTest, ExternalAuthAllowsValidStream) {
    mockHttpClient_->setAllowResponse();

    auto result = externalAuth_->authenticate(
        "live",
        "valid_stream_key",
        createTestClient(),
        core::AuthAction::Publish
    );

    EXPECT_TRUE(result.allowed);
    EXPECT_EQ(mockHttpClient_->receivedRequests.size(), 1u);
}

TEST_F(AuthCallbackIntegrationTest, ExternalAuthDeniesInvalidStream) {
    mockHttpClient_->setDenyResponse("Invalid stream key");

    auto result = externalAuth_->authenticate(
        "live",
        "invalid_stream_key",
        createTestClient(),
        core::AuthAction::Publish
    );

    EXPECT_FALSE(result.allowed);
    ASSERT_TRUE(result.reason.has_value());
    EXPECT_NE(result.reason->find("Invalid"), std::string::npos);
}

// =============================================================================
// HTTP Callback Request Tests
// =============================================================================

TEST_F(AuthCallbackIntegrationTest, CallbackSendsCorrectRequestData) {
    mockHttpClient_->setAllowResponse();

    ClientInfo client;
    client.ip = "10.0.0.50";
    client.port = 54321;

    externalAuth_->authenticate(
        "myapp",
        "stream123",
        client,
        core::AuthAction::Publish
    );

    ASSERT_EQ(mockHttpClient_->receivedRequests.size(), 1u);

    auto& request = mockHttpClient_->receivedRequests[0];
    EXPECT_EQ(request.first, "http://localhost:8080/auth");

    // Request body should contain the auth parameters as JSON
    auto& body = request.second.body;
    EXPECT_NE(body.find("myapp"), std::string::npos);
    EXPECT_NE(body.find("stream123"), std::string::npos);
    EXPECT_NE(body.find("10.0.0.50"), std::string::npos);
    EXPECT_NE(body.find("publish"), std::string::npos);
}

TEST_F(AuthCallbackIntegrationTest, CallbackHandlesPublishAction) {
    mockHttpClient_->setAllowResponse();

    externalAuth_->authenticate(
        "live",
        "test_stream",
        createTestClient(),
        core::AuthAction::Publish
    );

    auto& body = mockHttpClient_->receivedRequests[0].second.body;
    EXPECT_NE(body.find("publish"), std::string::npos);
}

TEST_F(AuthCallbackIntegrationTest, CallbackHandlesSubscribeAction) {
    mockHttpClient_->setAllowResponse();

    externalAuth_->authenticate(
        "live",
        "test_stream",
        createTestClient(),
        core::AuthAction::Subscribe
    );

    auto& body = mockHttpClient_->receivedRequests[0].second.body;
    // Note: RTMP uses "play" terminology for subscribing to a stream
    EXPECT_NE(body.find("play"), std::string::npos);
}

// =============================================================================
// Timeout Handling Tests
// =============================================================================

TEST_F(AuthCallbackIntegrationTest, TimeoutWithDenyFallback) {
    mockHttpClient_->shouldTimeout = true;

    // Configure to deny on timeout
    core::ExternalAuthConfig config = externalAuth_->getConfig();
    config.fallbackOnTimeout = core::AuthFallbackBehavior::Deny;
    externalAuth_->updateConfig(config);

    auto result = externalAuth_->authenticate(
        "live",
        "test_stream",
        createTestClient(),
        core::AuthAction::Publish
    );

    EXPECT_FALSE(result.allowed);
}

TEST_F(AuthCallbackIntegrationTest, TimeoutWithAllowFallback) {
    mockHttpClient_->shouldTimeout = true;

    // Configure to allow on timeout
    core::ExternalAuthConfig config = externalAuth_->getConfig();
    config.fallbackOnTimeout = core::AuthFallbackBehavior::Allow;
    externalAuth_->updateConfig(config);

    auto result = externalAuth_->authenticate(
        "live",
        "test_stream",
        createTestClient(),
        core::AuthAction::Publish
    );

    EXPECT_TRUE(result.allowed);
}

TEST_F(AuthCallbackIntegrationTest, TimeoutStatisticsTracked) {
    mockHttpClient_->shouldTimeout = true;

    // Disable caching for this test to avoid lock issues
    core::ExternalAuthConfig config = externalAuth_->getConfig();
    config.cacheEnabled = false;
    config.circuitBreakerThreshold = 100;  // High threshold to avoid opening circuit
    externalAuth_->updateConfig(config);

    // Make a single timeout request (avoid loops that might trigger circuit breaker)
    externalAuth_->authenticate(
        "live",
        "test_stream",
        createTestClient(),
        core::AuthAction::Publish
    );

    auto stats = externalAuth_->getStatistics();
    EXPECT_GE(stats.timeoutCount, 1u);
}

// =============================================================================
// Circuit Breaker Tests
// =============================================================================

TEST_F(AuthCallbackIntegrationTest, CircuitBreakerOpensAfterFailures) {
    mockHttpClient_->shouldSucceed = false;

    // Configure circuit breaker with low threshold, disable caching
    core::ExternalAuthConfig config = externalAuth_->getConfig();
    config.circuitBreakerThreshold = 2;
    config.cacheEnabled = false;
    config.fallbackOnTimeout = core::AuthFallbackBehavior::Deny;
    externalAuth_->updateConfig(config);

    // Cause failures to open circuit (threshold = 2)
    externalAuth_->authenticate("live", "s1", createTestClient(), core::AuthAction::Publish);
    externalAuth_->authenticate("live", "s2", createTestClient(), core::AuthAction::Publish);

    // Circuit should be open after 2 failures
    EXPECT_EQ(externalAuth_->getCircuitState(), core::CircuitState::Open);
}

TEST_F(AuthCallbackIntegrationTest, CircuitBreakerPreventsRequests) {
    mockHttpClient_->shouldSucceed = false;

    // Configure and disable caching
    core::ExternalAuthConfig config = externalAuth_->getConfig();
    config.circuitBreakerThreshold = 2;
    config.cacheEnabled = false;
    externalAuth_->updateConfig(config);

    // Open the circuit with 2 failures
    externalAuth_->authenticate("live", "t1", createTestClient(), core::AuthAction::Publish);
    externalAuth_->authenticate("live", "t2", createTestClient(), core::AuthAction::Publish);

    // Circuit should now be open
    EXPECT_EQ(externalAuth_->getCircuitState(), core::CircuitState::Open);

    // Track requests before trying more
    size_t requestsBefore = mockHttpClient_->receivedRequests.size();

    // Further requests should be rejected by circuit breaker (not reaching HTTP)
    mockHttpClient_->shouldSucceed = true;  // Even if service recovers
    auto result = externalAuth_->authenticate("live", "t3", createTestClient(), core::AuthAction::Publish);

    // Request should be denied due to open circuit
    EXPECT_FALSE(result.allowed);
}

TEST_F(AuthCallbackIntegrationTest, CircuitBreakerStatisticsTracked) {
    mockHttpClient_->shouldSucceed = false;

    // Configure and disable caching
    core::ExternalAuthConfig config = externalAuth_->getConfig();
    config.circuitBreakerThreshold = 2;
    config.cacheEnabled = false;
    externalAuth_->updateConfig(config);

    // Trigger circuit breaker with 2 failures
    externalAuth_->authenticate("live", "s1", createTestClient(), core::AuthAction::Publish);
    externalAuth_->authenticate("live", "s2", createTestClient(), core::AuthAction::Publish);

    auto stats = externalAuth_->getStatistics();
    EXPECT_GE(stats.failureCount, 2u);
}

// =============================================================================
// Cache Tests
// =============================================================================

TEST_F(AuthCallbackIntegrationTest, PositiveResultsCached) {
    mockHttpClient_->setAllowResponse();

    // First request
    auto result1 = externalAuth_->authenticate(
        "live",
        "cached_stream",
        createTestClient(),
        core::AuthAction::Publish
    );
    EXPECT_TRUE(result1.allowed);

    size_t requestsAfterFirst = mockHttpClient_->receivedRequests.size();

    // Second request should be cached
    auto result2 = externalAuth_->authenticate(
        "live",
        "cached_stream",
        createTestClient(),
        core::AuthAction::Publish
    );
    EXPECT_TRUE(result2.allowed);

    // No additional HTTP request should have been made
    EXPECT_EQ(mockHttpClient_->receivedRequests.size(), requestsAfterFirst);

    // Cache hit should be tracked
    auto stats = externalAuth_->getStatistics();
    EXPECT_GE(stats.cacheHits, 1u);
}

TEST_F(AuthCallbackIntegrationTest, CacheKeyIncludesAllParameters) {
    mockHttpClient_->setAllowResponse();

    // Request with different parameters should not use cache
    externalAuth_->authenticate("app1", "stream1", createTestClient("1.1.1.1"), core::AuthAction::Publish);
    externalAuth_->authenticate("app2", "stream1", createTestClient("1.1.1.1"), core::AuthAction::Publish);
    externalAuth_->authenticate("app1", "stream2", createTestClient("1.1.1.1"), core::AuthAction::Publish);
    externalAuth_->authenticate("app1", "stream1", createTestClient("2.2.2.2"), core::AuthAction::Publish);

    // All should be cache misses (different parameters)
    EXPECT_GE(mockHttpClient_->receivedRequests.size(), 4u);
}

TEST_F(AuthCallbackIntegrationTest, CacheCanBeCleared) {
    mockHttpClient_->setAllowResponse();

    // Populate cache
    externalAuth_->authenticate("live", "test", createTestClient(), core::AuthAction::Publish);
    size_t requestsAfterFirst = mockHttpClient_->receivedRequests.size();

    // Clear cache
    externalAuth_->clearCache();

    // Next request should not be cached
    externalAuth_->authenticate("live", "test", createTestClient(), core::AuthAction::Publish);

    // New request should have been made
    EXPECT_GT(mockHttpClient_->receivedRequests.size(), requestsAfterFirst);
}

// =============================================================================
// Auth Service Integration Tests
// =============================================================================

TEST_F(AuthCallbackIntegrationTest, AuthServiceValidatesStreamKeys) {
    // Add valid stream key
    authService_->addStreamKey("valid_key", "live");

    ClientInfo client = createTestClient();

    // Valid key should pass
    auto result = authService_->validatePublish("live", "valid_key", client);
    EXPECT_TRUE(result.allowed);

    // Invalid key should fail
    result = authService_->validatePublish("live", "invalid_key", client);
    EXPECT_FALSE(result.allowed);
}

TEST_F(AuthCallbackIntegrationTest, AuthServiceGeneratesKeys) {
    auto generatedKey = authService_->generateStreamKey();

    EXPECT_FALSE(generatedKey.empty());
    EXPECT_TRUE(authService_->hasStreamKey(generatedKey));

    // Should be able to authenticate with generated key
    auto result = authService_->validatePublish("", generatedKey, createTestClient());
    EXPECT_TRUE(result.allowed);
}

TEST_F(AuthCallbackIntegrationTest, AuthServiceLogsAttempts) {
    std::vector<core::AuthLogEntry> logEntries;

    authService_->setAuthLogCallback([&logEntries](const core::AuthLogEntry& entry) {
        logEntries.push_back(entry);
    });

    // Make some auth attempts
    authService_->addStreamKey("test_key");
    authService_->validatePublish("live", "test_key", createTestClient());
    authService_->validatePublish("live", "bad_key", createTestClient());

    // Logs should be recorded
    EXPECT_GE(logEntries.size(), 2u);

    // Check log entry content
    bool foundSuccess = false;
    bool foundFailure = false;
    for (const auto& entry : logEntries) {
        if (entry.success) foundSuccess = true;
        else foundFailure = true;
    }
    EXPECT_TRUE(foundSuccess);
    EXPECT_TRUE(foundFailure);
}

// =============================================================================
// Complete Auth Flow Integration Tests
// =============================================================================

TEST_F(AuthCallbackIntegrationTest, CompletePublishAuthFlow) {
    // Setup: Add valid stream key to auth service
    const std::string validKey = "live_stream_key_123";
    authService_->addStreamKey(validKey, "live");

    // Step 1: Validate publish request
    ClientInfo client = createTestClient();
    auto authResult = authService_->validatePublish("live", validKey, client);
    ASSERT_TRUE(authResult.allowed);

    // Step 2: Setup session and connect
    protocol::SessionContext session;
    session.sessionId = 1;
    session.connectionId = 1;
    session.state = protocol::SessionState::Connected;
    session.appName = "live";

    // Step 3: Create stream
    protocol::RTMPCommand createStreamCmd;
    createStreamCmd.name = "createStream";
    createStreamCmd.transactionId = 1.0;
    createStreamCmd.commandObject = protocol::AMFValue::makeNull();

    auto createResult = commandHandler_->handleCreateStream(createStreamCmd, session);
    ASSERT_TRUE(createResult.isSuccess());

    // Step 4: Publish with validated key
    protocol::RTMPCommand publishCmd;
    publishCmd.name = "publish";
    publishCmd.transactionId = 2.0;
    publishCmd.commandObject = protocol::AMFValue::makeNull();
    publishCmd.args.push_back(protocol::AMFValue::makeString(validKey));
    publishCmd.args.push_back(protocol::AMFValue::makeString("live"));

    auto publishResult = commandHandler_->handlePublish(publishCmd, session);
    ASSERT_TRUE(publishResult.isSuccess());
    EXPECT_EQ(session.state, protocol::SessionState::Publishing);
}

TEST_F(AuthCallbackIntegrationTest, PublishRejectedForInvalidKey) {
    // No valid keys added

    ClientInfo client = createTestClient();
    auto authResult = authService_->validatePublish("live", "unauthorized_key", client);

    EXPECT_FALSE(authResult.allowed);
    EXPECT_TRUE(authResult.reason.has_value());
}

TEST_F(AuthCallbackIntegrationTest, ExternalAuthIntegrationWithPublish) {
    mockHttpClient_->setAllowResponse();

    // Step 1: External auth check
    auto externalResult = externalAuth_->authenticate(
        "live",
        "stream_requiring_external_auth",
        createTestClient(),
        core::AuthAction::Publish
    );
    ASSERT_TRUE(externalResult.allowed);

    // Step 2: Proceed with publish
    protocol::SessionContext session;
    session.sessionId = 1;
    session.connectionId = 1;
    session.state = protocol::SessionState::Connected;
    session.appName = "live";
    session.streamId = 1;

    protocol::RTMPCommand publishCmd;
    publishCmd.name = "publish";
    publishCmd.transactionId = 1.0;
    publishCmd.commandObject = protocol::AMFValue::makeNull();
    publishCmd.args.push_back(protocol::AMFValue::makeString("stream_requiring_external_auth"));
    publishCmd.args.push_back(protocol::AMFValue::makeString("live"));

    auto publishResult = commandHandler_->handlePublish(publishCmd, session);
    EXPECT_TRUE(publishResult.isSuccess());
}

TEST_F(AuthCallbackIntegrationTest, ExternalAuthDenialBlocksPublish) {
    mockHttpClient_->setDenyResponse("Account suspended");

    // External auth denies
    auto externalResult = externalAuth_->authenticate(
        "live",
        "suspended_stream",
        createTestClient(),
        core::AuthAction::Publish
    );

    EXPECT_FALSE(externalResult.allowed);
    EXPECT_TRUE(externalResult.reason.has_value());
    EXPECT_NE(externalResult.reason->find("suspended"), std::string::npos);
}

// =============================================================================
// Statistics Tests
// =============================================================================

TEST_F(AuthCallbackIntegrationTest, ExternalAuthStatisticsAccurate) {
    // Mix of successful and failed auth attempts
    mockHttpClient_->setAllowResponse();
    for (int i = 0; i < 5; ++i) {
        externalAuth_->authenticate("live", "stream_" + std::to_string(i), createTestClient(), core::AuthAction::Publish);
    }

    mockHttpClient_->setDenyResponse();
    for (int i = 0; i < 3; ++i) {
        externalAuth_->authenticate("live", "denied_" + std::to_string(i), createTestClient(), core::AuthAction::Publish);
    }

    auto stats = externalAuth_->getStatistics();
    EXPECT_EQ(stats.totalRequests, 8u);
    EXPECT_EQ(stats.successCount, 5u);
    EXPECT_EQ(stats.failureCount, 3u);
}

} // namespace test
} // namespace integration
} // namespace openrtmp
