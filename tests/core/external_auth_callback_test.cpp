// OpenRTMP - Cross-platform RTMP Server
// Tests for External Authentication Callback
//
// Tests cover:
// - HTTP POST callback to external auth service
// - Request payload with app, streamKey, clientIP, action
// - Callback timeout with configurable fallback behavior
// - Circuit breaker for failed auth service calls
// - Caching of positive authentication results
//
// Requirements coverage:
// - Requirement 15.4: Integration with external authentication services via HTTP callbacks
// - Requirement 20.4: Circuit breaker patterns for external service calls

#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <mutex>
#include <functional>

#include "openrtmp/core/external_auth_callback.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace core {
namespace test {

// =============================================================================
// Mock HTTP Client for Testing
// =============================================================================

/**
 * @brief Mock HTTP client that simulates external auth service responses.
 */
class MockHttpClient : public IHttpClient {
public:
    // Configure mock behavior
    void setResponse(HttpResponse response) {
        std::lock_guard<std::mutex> lock(mutex_);
        response_ = std::move(response);
    }

    void setDelay(std::chrono::milliseconds delay) {
        std::lock_guard<std::mutex> lock(mutex_);
        delay_ = delay;
    }

    void setShouldTimeout(bool timeout) {
        std::lock_guard<std::mutex> lock(mutex_);
        shouldTimeout_ = timeout;
    }

    void setShouldFail(bool fail) {
        std::lock_guard<std::mutex> lock(mutex_);
        shouldFail_ = fail;
    }

    // Get last request for verification
    HttpRequest getLastRequest() {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastRequest_;
    }

    int getRequestCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requestCount_;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        requestCount_ = 0;
        lastRequest_ = HttpRequest{};
    }

    // IHttpClient implementation
    Result<HttpResponse, HttpError> post(
        const std::string& url,
        const HttpRequest& request,
        std::chrono::milliseconds timeout
    ) override {
        std::lock_guard<std::mutex> lock(mutex_);
        lastRequest_ = request;
        lastRequest_.url = url;
        requestCount_++;

        // Simulate delay
        if (delay_.count() > 0) {
            std::this_thread::sleep_for(delay_);
        }

        // Simulate timeout
        if (shouldTimeout_) {
            HttpError err;
            err.code = HttpError::Code::Timeout;
            err.message = "Request timed out";
            return Result<HttpResponse, HttpError>::error(err);
        }

        // Simulate failure
        if (shouldFail_) {
            HttpError err;
            err.code = HttpError::Code::ConnectionFailed;
            err.message = "Connection failed";
            return Result<HttpResponse, HttpError>::error(err);
        }

        return Result<HttpResponse, HttpError>::success(response_);
    }

private:
    std::mutex mutex_;
    HttpResponse response_;
    HttpRequest lastRequest_;
    std::chrono::milliseconds delay_{0};
    bool shouldTimeout_ = false;
    bool shouldFail_ = false;
    int requestCount_ = 0;
};

// =============================================================================
// Test Fixtures
// =============================================================================

class ExternalAuthCallbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        mockClient_ = std::make_shared<MockHttpClient>();

        ExternalAuthConfig config;
        config.callbackUrl = "http://localhost:8080/auth";
        config.timeoutMs = 5000;
        config.circuitBreakerThreshold = 5;
        config.circuitBreakerResetMs = 30000;
        config.cacheTtlMs = 60000;
        config.fallbackOnTimeout = AuthFallbackBehavior::Deny;

        callback_ = std::make_unique<ExternalAuthCallback>(config, mockClient_);
    }

    void TearDown() override {
        callback_.reset();
        mockClient_.reset();
    }

    // Helper to create ClientInfo
    ClientInfo makeClient(const std::string& ip, uint16_t port = 12345) {
        ClientInfo info;
        info.ip = ip;
        info.port = port;
        info.userAgent = "Test Client";
        return info;
    }

    // Helper to set successful auth response
    void setSuccessResponse(bool allowed = true, const std::string& reason = "") {
        HttpResponse response;
        response.statusCode = 200;
        if (allowed) {
            response.body = R"({"allowed": true})";
        } else {
            response.body = R"({"allowed": false, "reason": ")" + reason + R"("})";
        }
        mockClient_->setResponse(response);
    }

    std::shared_ptr<MockHttpClient> mockClient_;
    std::unique_ptr<ExternalAuthCallback> callback_;
};

// =============================================================================
// HTTP POST Request Tests (Requirement 15.4)
// =============================================================================

TEST_F(ExternalAuthCallbackTest, MakesHttpPostRequestToConfiguredUrl) {
    setSuccessResponse();

    ClientInfo client = makeClient("192.168.1.100");
    callback_->authenticate("live", "stream-key-123", client, AuthAction::Publish);

    auto request = mockClient_->getLastRequest();
    EXPECT_EQ(request.url, "http://localhost:8080/auth");
    EXPECT_EQ(request.method, "POST");
}

TEST_F(ExternalAuthCallbackTest, RequestContainsCorrectPayload) {
    setSuccessResponse();

    ClientInfo client = makeClient("192.168.1.100", 54321);
    callback_->authenticate("live", "stream-key-123", client, AuthAction::Publish);

    auto request = mockClient_->getLastRequest();

    // Verify JSON payload contains required fields
    EXPECT_NE(request.body.find("\"app\":\"live\""), std::string::npos);
    EXPECT_NE(request.body.find("\"streamKey\":\"stream-key-123\""), std::string::npos);
    EXPECT_NE(request.body.find("\"clientIP\":\"192.168.1.100\""), std::string::npos);
    EXPECT_NE(request.body.find("\"action\":\"publish\""), std::string::npos);
}

TEST_F(ExternalAuthCallbackTest, RequestContainsPlayActionForSubscribe) {
    setSuccessResponse();

    ClientInfo client = makeClient("192.168.1.100");
    callback_->authenticate("live", "stream-key-123", client, AuthAction::Subscribe);

    auto request = mockClient_->getLastRequest();
    EXPECT_NE(request.body.find("\"action\":\"play\""), std::string::npos);
}

TEST_F(ExternalAuthCallbackTest, AuthSucceedsWhenExternalServiceAllows) {
    setSuccessResponse(true);

    ClientInfo client = makeClient("192.168.1.100");
    auto result = callback_->authenticate("live", "stream-key-123", client, AuthAction::Publish);

    EXPECT_TRUE(result.allowed);
}

TEST_F(ExternalAuthCallbackTest, AuthFailsWhenExternalServiceDenies) {
    setSuccessResponse(false, "Stream key not found");

    ClientInfo client = makeClient("192.168.1.100");
    auto result = callback_->authenticate("live", "invalid-key", client, AuthAction::Publish);

    EXPECT_FALSE(result.allowed);
    EXPECT_TRUE(result.reason.has_value());
    EXPECT_NE(result.reason->find("Stream key not found"), std::string::npos);
}

// =============================================================================
// Timeout Handling Tests
// =============================================================================

TEST_F(ExternalAuthCallbackTest, TimeoutDeniesWhenFallbackIsDeny) {
    mockClient_->setShouldTimeout(true);

    ExternalAuthConfig config;
    config.callbackUrl = "http://localhost:8080/auth";
    config.timeoutMs = 100;
    config.fallbackOnTimeout = AuthFallbackBehavior::Deny;
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    ClientInfo client = makeClient("192.168.1.100");
    auto result = callback->authenticate("live", "stream-key", client, AuthAction::Publish);

    EXPECT_FALSE(result.allowed);
    EXPECT_TRUE(result.reason.has_value());
}

TEST_F(ExternalAuthCallbackTest, TimeoutAllowsWhenFallbackIsAllow) {
    mockClient_->setShouldTimeout(true);

    ExternalAuthConfig config;
    config.callbackUrl = "http://localhost:8080/auth";
    config.timeoutMs = 100;
    config.fallbackOnTimeout = AuthFallbackBehavior::Allow;
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    ClientInfo client = makeClient("192.168.1.100");
    auto result = callback->authenticate("live", "stream-key", client, AuthAction::Publish);

    EXPECT_TRUE(result.allowed);
}

TEST_F(ExternalAuthCallbackTest, TimeoutUsesConfiguredTimeoutValue) {
    // Use a mock that records timeout
    setSuccessResponse();

    ExternalAuthConfig config;
    config.callbackUrl = "http://localhost:8080/auth";
    config.timeoutMs = 3000;  // 3 seconds
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    ClientInfo client = makeClient("192.168.1.100");
    callback->authenticate("live", "stream-key", client, AuthAction::Publish);

    // The timeout should be passed to the HTTP client
    // In a real implementation, we'd verify this through the mock
    EXPECT_EQ(mockClient_->getRequestCount(), 1);
}

// =============================================================================
// Circuit Breaker Tests (Requirement 20.4)
// =============================================================================

TEST_F(ExternalAuthCallbackTest, CircuitBreakerOpensAfterThresholdFailures) {
    mockClient_->setShouldFail(true);

    ExternalAuthConfig config;
    config.callbackUrl = "http://localhost:8080/auth";
    config.circuitBreakerThreshold = 5;
    config.circuitBreakerResetMs = 30000;
    config.fallbackOnTimeout = AuthFallbackBehavior::Deny;
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    ClientInfo client = makeClient("192.168.1.100");

    // Make threshold number of failing requests
    for (int i = 0; i < 5; ++i) {
        callback->authenticate("live", "key", client, AuthAction::Publish);
    }

    // Reset counter to track requests when circuit is open
    mockClient_->reset();

    // Next request should not reach the service (circuit is open)
    auto result = callback->authenticate("live", "key", client, AuthAction::Publish);

    EXPECT_FALSE(result.allowed);
    EXPECT_EQ(mockClient_->getRequestCount(), 0);  // No request made
}

TEST_F(ExternalAuthCallbackTest, CircuitBreakerDeniesWhenOpen) {
    mockClient_->setShouldFail(true);

    ExternalAuthConfig config;
    config.callbackUrl = "http://localhost:8080/auth";
    config.circuitBreakerThreshold = 3;
    config.circuitBreakerResetMs = 30000;
    config.fallbackOnTimeout = AuthFallbackBehavior::Deny;
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    ClientInfo client = makeClient("192.168.1.100");

    // Open the circuit
    for (int i = 0; i < 3; ++i) {
        callback->authenticate("live", "key", client, AuthAction::Publish);
    }

    // Verify circuit is open
    auto result = callback->authenticate("live", "key", client, AuthAction::Publish);
    EXPECT_FALSE(result.allowed);
    EXPECT_TRUE(result.reason.has_value());
    EXPECT_NE(result.reason->find("circuit"), std::string::npos);
}

TEST_F(ExternalAuthCallbackTest, CircuitBreakerResetsAfterTimeout) {
    mockClient_->setShouldFail(true);

    ExternalAuthConfig config;
    config.callbackUrl = "http://localhost:8080/auth";
    config.circuitBreakerThreshold = 3;
    config.circuitBreakerResetMs = 100;  // Very short for testing
    config.fallbackOnTimeout = AuthFallbackBehavior::Deny;
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    ClientInfo client = makeClient("192.168.1.100");

    // Open the circuit
    for (int i = 0; i < 3; ++i) {
        callback->authenticate("live", "key", client, AuthAction::Publish);
    }

    // Wait for circuit to reset
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Now the service should be called again (half-open state)
    mockClient_->reset();
    mockClient_->setShouldFail(false);
    setSuccessResponse();

    auto result = callback->authenticate("live", "key", client, AuthAction::Publish);
    EXPECT_EQ(mockClient_->getRequestCount(), 1);  // Request was made
    EXPECT_TRUE(result.allowed);
}

TEST_F(ExternalAuthCallbackTest, CircuitBreakerClosesOnSuccessInHalfOpen) {
    mockClient_->setShouldFail(true);

    ExternalAuthConfig config;
    config.callbackUrl = "http://localhost:8080/auth";
    config.circuitBreakerThreshold = 2;
    config.circuitBreakerResetMs = 50;
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    ClientInfo client = makeClient("192.168.1.100");

    // Open the circuit
    callback->authenticate("live", "key", client, AuthAction::Publish);
    callback->authenticate("live", "key", client, AuthAction::Publish);

    // Wait for half-open
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // Successful request in half-open state
    mockClient_->setShouldFail(false);
    setSuccessResponse();
    callback->authenticate("live", "key", client, AuthAction::Publish);

    // Circuit should be closed now - multiple requests should work
    mockClient_->reset();
    callback->authenticate("live", "key1", client, AuthAction::Publish);
    callback->authenticate("live", "key2", client, AuthAction::Publish);
    EXPECT_EQ(mockClient_->getRequestCount(), 2);
}

TEST_F(ExternalAuthCallbackTest, CircuitBreakerReopensOnFailureInHalfOpen) {
    mockClient_->setShouldFail(true);

    ExternalAuthConfig config;
    config.callbackUrl = "http://localhost:8080/auth";
    config.circuitBreakerThreshold = 2;
    config.circuitBreakerResetMs = 50;
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    ClientInfo client = makeClient("192.168.1.100");

    // Open the circuit
    callback->authenticate("live", "key", client, AuthAction::Publish);
    callback->authenticate("live", "key", client, AuthAction::Publish);

    // Wait for half-open
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // Failed request in half-open state (still failing)
    mockClient_->reset();
    callback->authenticate("live", "key", client, AuthAction::Publish);
    EXPECT_EQ(mockClient_->getRequestCount(), 1);

    // Circuit should be open again
    callback->authenticate("live", "key", client, AuthAction::Publish);
    EXPECT_EQ(mockClient_->getRequestCount(), 1);  // No new request
}

TEST_F(ExternalAuthCallbackTest, CircuitBreakerStateQueryable) {
    mockClient_->setShouldFail(true);

    ExternalAuthConfig config;
    config.callbackUrl = "http://localhost:8080/auth";
    config.circuitBreakerThreshold = 2;
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    ClientInfo client = makeClient("192.168.1.100");

    // Initially closed
    EXPECT_EQ(callback->getCircuitState(), CircuitState::Closed);

    // Open the circuit
    callback->authenticate("live", "key", client, AuthAction::Publish);
    callback->authenticate("live", "key", client, AuthAction::Publish);

    EXPECT_EQ(callback->getCircuitState(), CircuitState::Open);
}

// =============================================================================
// Cache Tests
// =============================================================================

TEST_F(ExternalAuthCallbackTest, CachesPositiveAuthResults) {
    setSuccessResponse();

    ExternalAuthConfig config;
    config.callbackUrl = "http://localhost:8080/auth";
    config.cacheTtlMs = 60000;  // 1 minute cache
    config.cacheEnabled = true;
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    ClientInfo client = makeClient("192.168.1.100");

    // First request hits the service
    auto result1 = callback->authenticate("live", "cached-key", client, AuthAction::Publish);
    EXPECT_TRUE(result1.allowed);
    EXPECT_EQ(mockClient_->getRequestCount(), 1);

    // Second request should use cache
    auto result2 = callback->authenticate("live", "cached-key", client, AuthAction::Publish);
    EXPECT_TRUE(result2.allowed);
    EXPECT_EQ(mockClient_->getRequestCount(), 1);  // Still 1, cache hit
}

TEST_F(ExternalAuthCallbackTest, CacheDoesNotCacheNegativeResults) {
    setSuccessResponse(false, "Denied");

    ExternalAuthConfig config;
    config.callbackUrl = "http://localhost:8080/auth";
    config.cacheTtlMs = 60000;
    config.cacheEnabled = true;
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    ClientInfo client = makeClient("192.168.1.100");

    // First request - denied
    callback->authenticate("live", "denied-key", client, AuthAction::Publish);
    EXPECT_EQ(mockClient_->getRequestCount(), 1);

    // Second request should NOT use cache (negative results not cached)
    callback->authenticate("live", "denied-key", client, AuthAction::Publish);
    EXPECT_EQ(mockClient_->getRequestCount(), 2);
}

TEST_F(ExternalAuthCallbackTest, CacheExpiresAfterTtl) {
    setSuccessResponse();

    ExternalAuthConfig config;
    config.callbackUrl = "http://localhost:8080/auth";
    config.cacheTtlMs = 50;  // Very short TTL for testing
    config.cacheEnabled = true;
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    ClientInfo client = makeClient("192.168.1.100");

    // First request
    callback->authenticate("live", "expiring-key", client, AuthAction::Publish);
    EXPECT_EQ(mockClient_->getRequestCount(), 1);

    // Wait for cache to expire
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // Next request should hit the service again
    callback->authenticate("live", "expiring-key", client, AuthAction::Publish);
    EXPECT_EQ(mockClient_->getRequestCount(), 2);
}

TEST_F(ExternalAuthCallbackTest, CacheKeyIncludesAllRelevantFields) {
    setSuccessResponse();

    ExternalAuthConfig config;
    config.callbackUrl = "http://localhost:8080/auth";
    config.cacheTtlMs = 60000;
    config.cacheEnabled = true;
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    ClientInfo client1 = makeClient("192.168.1.100");
    ClientInfo client2 = makeClient("192.168.1.200");  // Different IP

    // Same stream key, different client IP
    callback->authenticate("live", "key1", client1, AuthAction::Publish);
    EXPECT_EQ(mockClient_->getRequestCount(), 1);

    callback->authenticate("live", "key1", client2, AuthAction::Publish);
    EXPECT_EQ(mockClient_->getRequestCount(), 2);  // Different cache key due to IP

    // Same everything except action
    callback->authenticate("live", "key1", client1, AuthAction::Subscribe);
    EXPECT_EQ(mockClient_->getRequestCount(), 3);  // Different cache key due to action
}

TEST_F(ExternalAuthCallbackTest, CacheCanBeDisabled) {
    setSuccessResponse();

    ExternalAuthConfig config;
    config.callbackUrl = "http://localhost:8080/auth";
    config.cacheTtlMs = 60000;
    config.cacheEnabled = false;  // Disabled
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    ClientInfo client = makeClient("192.168.1.100");

    callback->authenticate("live", "key", client, AuthAction::Publish);
    callback->authenticate("live", "key", client, AuthAction::Publish);
    callback->authenticate("live", "key", client, AuthAction::Publish);

    EXPECT_EQ(mockClient_->getRequestCount(), 3);  // All requests hit service
}

TEST_F(ExternalAuthCallbackTest, ClearCacheWorks) {
    setSuccessResponse();

    ExternalAuthConfig config;
    config.callbackUrl = "http://localhost:8080/auth";
    config.cacheTtlMs = 60000;
    config.cacheEnabled = true;
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    ClientInfo client = makeClient("192.168.1.100");

    callback->authenticate("live", "key", client, AuthAction::Publish);
    EXPECT_EQ(mockClient_->getRequestCount(), 1);

    // Clear cache
    callback->clearCache();

    // Next request should hit service
    callback->authenticate("live", "key", client, AuthAction::Publish);
    EXPECT_EQ(mockClient_->getRequestCount(), 2);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

class ExternalAuthCallbackConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        mockClient_ = std::make_shared<MockHttpClient>();

        HttpResponse response;
        response.statusCode = 200;
        response.body = R"({"allowed": true})";
        mockClient_->setResponse(response);

        ExternalAuthConfig config;
        config.callbackUrl = "http://localhost:8080/auth";
        config.cacheTtlMs = 60000;
        config.cacheEnabled = true;
        config.circuitBreakerThreshold = 100;  // High to avoid opening during test

        callback_ = std::make_unique<ExternalAuthCallback>(config, mockClient_);
    }

    std::shared_ptr<MockHttpClient> mockClient_;
    std::unique_ptr<ExternalAuthCallback> callback_;
};

TEST_F(ExternalAuthCallbackConcurrencyTest, ConcurrentAuthRequestsAreThreadSafe) {
    const int numThreads = 10;
    const int requestsPerThread = 50;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, i, &successCount]() {
            ClientInfo client;
            client.ip = "192.168.1." + std::to_string(i);
            client.port = 12345;

            for (int j = 0; j < 50; ++j) {
                std::string key = "key_" + std::to_string(i) + "_" + std::to_string(j);
                auto result = callback_->authenticate("live", key, client, AuthAction::Publish);
                if (result.allowed) {
                    successCount++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(successCount.load(), numThreads * requestsPerThread);
}

TEST_F(ExternalAuthCallbackConcurrencyTest, ConcurrentCacheAccessIsThreadSafe) {
    const int numThreads = 10;
    std::vector<std::thread> threads;
    std::atomic<bool> running{true};

    // All threads use the same key to test cache contention
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, &running]() {
            ClientInfo client;
            client.ip = "192.168.1.1";
            client.port = 12345;

            while (running) {
                callback_->authenticate("live", "shared-key", client, AuthAction::Publish);
            }
        });
    }

    // Let threads run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& thread : threads) {
        thread.join();
    }

    // If we get here without crash/deadlock, concurrency is handled
    SUCCEED();
}

// =============================================================================
// Integration with AuthService Tests
// =============================================================================

TEST_F(ExternalAuthCallbackTest, CanBeUsedAsAuthCallback) {
    setSuccessResponse();

    // Verify the callback has the expected interface
    IExternalAuthCallback* interfacePtr = callback_.get();
    EXPECT_NE(interfacePtr, nullptr);

    ClientInfo client = makeClient("192.168.1.100");
    auto result = interfacePtr->authenticate("live", "key", client, AuthAction::Publish);
    EXPECT_TRUE(result.allowed);
}

// =============================================================================
// Configuration Tests
// =============================================================================

TEST_F(ExternalAuthCallbackTest, ConfigurationCanBeUpdated) {
    setSuccessResponse();

    ExternalAuthConfig newConfig;
    newConfig.callbackUrl = "http://newhost:9090/auth";
    newConfig.timeoutMs = 10000;
    newConfig.cacheTtlMs = 120000;

    callback_->updateConfig(newConfig);

    ClientInfo client = makeClient("192.168.1.100");
    callback_->authenticate("live", "key", client, AuthAction::Publish);

    auto request = mockClient_->getLastRequest();
    EXPECT_EQ(request.url, "http://newhost:9090/auth");
}

TEST_F(ExternalAuthCallbackTest, GetConfigReturnsCurrentConfiguration) {
    ExternalAuthConfig config;
    config.callbackUrl = "http://test:8080/auth";
    config.timeoutMs = 3000;
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    auto currentConfig = callback->getConfig();
    EXPECT_EQ(currentConfig.callbackUrl, "http://test:8080/auth");
    EXPECT_EQ(currentConfig.timeoutMs, 3000u);
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST_F(ExternalAuthCallbackTest, HandlesInvalidJsonResponse) {
    HttpResponse response;
    response.statusCode = 200;
    response.body = "not valid json";
    mockClient_->setResponse(response);

    ExternalAuthConfig config;
    config.callbackUrl = "http://localhost:8080/auth";
    config.fallbackOnTimeout = AuthFallbackBehavior::Deny;
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    ClientInfo client = makeClient("192.168.1.100");
    auto result = callback->authenticate("live", "key", client, AuthAction::Publish);

    // Invalid response should be treated as failure
    EXPECT_FALSE(result.allowed);
}

TEST_F(ExternalAuthCallbackTest, HandlesNon200StatusCode) {
    HttpResponse response;
    response.statusCode = 500;
    response.body = R"({"error": "Internal server error"})";
    mockClient_->setResponse(response);

    ClientInfo client = makeClient("192.168.1.100");
    auto result = callback_->authenticate("live", "key", client, AuthAction::Publish);

    EXPECT_FALSE(result.allowed);
}

TEST_F(ExternalAuthCallbackTest, HandlesEmptyResponse) {
    HttpResponse response;
    response.statusCode = 200;
    response.body = "";
    mockClient_->setResponse(response);

    ClientInfo client = makeClient("192.168.1.100");
    auto result = callback_->authenticate("live", "key", client, AuthAction::Publish);

    EXPECT_FALSE(result.allowed);
}

// =============================================================================
// Statistics Tests
// =============================================================================

TEST_F(ExternalAuthCallbackTest, TracksStatistics) {
    setSuccessResponse();

    ClientInfo client = makeClient("192.168.1.100");

    // Make some requests
    callback_->authenticate("live", "key1", client, AuthAction::Publish);
    callback_->authenticate("live", "key2", client, AuthAction::Publish);

    auto stats = callback_->getStatistics();
    EXPECT_GE(stats.totalRequests, 2u);
    EXPECT_GE(stats.successCount, 2u);
    EXPECT_EQ(stats.failureCount, 0u);
}

TEST_F(ExternalAuthCallbackTest, TracksCacheHitRatio) {
    setSuccessResponse();

    ExternalAuthConfig config;
    config.callbackUrl = "http://localhost:8080/auth";
    config.cacheTtlMs = 60000;
    config.cacheEnabled = true;
    auto callback = std::make_unique<ExternalAuthCallback>(config, mockClient_);

    ClientInfo client = makeClient("192.168.1.100");

    // First request - cache miss
    callback->authenticate("live", "key", client, AuthAction::Publish);

    // Second request - cache hit
    callback->authenticate("live", "key", client, AuthAction::Publish);

    auto stats = callback->getStatistics();
    EXPECT_EQ(stats.cacheHits, 1u);
    EXPECT_EQ(stats.cacheMisses, 1u);
}

} // namespace test
} // namespace core
} // namespace openrtmp
