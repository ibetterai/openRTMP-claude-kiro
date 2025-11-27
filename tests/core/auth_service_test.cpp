// OpenRTMP - Cross-platform RTMP Server
// Tests for Authentication Service
//
// Tests cover:
// - Validate stream keys against configured allow list
// - Support dynamic stream key generation and revocation
// - Integrate with command processor for publish/play validation
// - Log authentication attempts with client IP and outcome
// - Return appropriate error responses for authentication failures
// - Optional subscribe authentication
//
// Requirements coverage:
// - Requirement 15.1: Stream key-based authentication for publish
// - Requirement 15.2: Validate stream keys against configured list
// - Requirement 15.3: Optional authentication for play/subscribe
// - Requirement 15.5: Reject with error and log on auth failure

#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <mutex>

#include "openrtmp/core/auth_service.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace core {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class AuthServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        AuthConfig config;
        config.publishAuthEnabled = true;
        config.subscribeAuthEnabled = false;  // Optional by default
        authService_ = std::make_unique<AuthService>(config);
    }

    void TearDown() override {
        authService_.reset();
    }

    std::unique_ptr<AuthService> authService_;

    // Helper to create ClientInfo
    ClientInfo makeClient(const std::string& ip, uint16_t port = 12345) {
        ClientInfo info;
        info.ip = ip;
        info.port = port;
        info.userAgent = "Test Client";
        return info;
    }
};

// =============================================================================
// Stream Key Validation Tests (Requirement 15.1, 15.2)
// =============================================================================

TEST_F(AuthServiceTest, ValidatePublishWithValidStreamKeySucceeds) {
    // Add a valid stream key to the allow list
    authService_->addStreamKey("test-stream-key-123");

    ClientInfo client = makeClient("192.168.1.100");
    auto result = authService_->validatePublish("live", "test-stream-key-123", client);

    EXPECT_TRUE(result.allowed);
    EXPECT_FALSE(result.reason.has_value());
}

TEST_F(AuthServiceTest, ValidatePublishWithInvalidStreamKeyFails) {
    // Add a valid stream key
    authService_->addStreamKey("valid-key");

    ClientInfo client = makeClient("192.168.1.100");
    auto result = authService_->validatePublish("live", "invalid-key", client);

    EXPECT_FALSE(result.allowed);
    EXPECT_TRUE(result.reason.has_value());
    EXPECT_NE(result.reason->find("Invalid"), std::string::npos);
}

TEST_F(AuthServiceTest, ValidatePublishWithEmptyStreamKeyFails) {
    ClientInfo client = makeClient("192.168.1.100");
    auto result = authService_->validatePublish("live", "", client);

    EXPECT_FALSE(result.allowed);
    EXPECT_TRUE(result.reason.has_value());
}

TEST_F(AuthServiceTest, ValidatePublishWithNoConfiguredKeysAllowsAll) {
    // Create auth service with publish auth disabled
    AuthConfig config;
    config.publishAuthEnabled = false;
    auto authService = std::make_unique<AuthService>(config);

    ClientInfo client = makeClient("192.168.1.100");
    auto result = authService->validatePublish("live", "any-key", client);

    EXPECT_TRUE(result.allowed);
}

TEST_F(AuthServiceTest, ValidatePublishWithEmptyAllowListDeniesAll) {
    // Auth is enabled but no keys configured
    ClientInfo client = makeClient("192.168.1.100");
    auto result = authService_->validatePublish("live", "any-key", client);

    EXPECT_FALSE(result.allowed);
    EXPECT_TRUE(result.reason.has_value());
}

// =============================================================================
// Subscribe Authentication Tests (Requirement 15.3)
// =============================================================================

TEST_F(AuthServiceTest, ValidateSubscribeWithAuthDisabledAllowsAll) {
    // By default, subscribe auth is disabled
    ClientInfo client = makeClient("192.168.1.100");
    auto result = authService_->validateSubscribe("live", "any-stream", client);

    EXPECT_TRUE(result.allowed);
}

TEST_F(AuthServiceTest, ValidateSubscribeWithAuthEnabledRequiresValidKey) {
    // Enable subscribe authentication
    AuthConfig config;
    config.publishAuthEnabled = true;
    config.subscribeAuthEnabled = true;
    auto authService = std::make_unique<AuthService>(config);
    authService->addStreamKey("subscriber-key");

    ClientInfo client = makeClient("192.168.1.100");

    // With invalid key
    auto result1 = authService->validateSubscribe("live", "invalid-key", client);
    EXPECT_FALSE(result1.allowed);

    // With valid key
    auto result2 = authService->validateSubscribe("live", "subscriber-key", client);
    EXPECT_TRUE(result2.allowed);
}

// =============================================================================
// Dynamic Stream Key Management Tests
// =============================================================================

TEST_F(AuthServiceTest, AddStreamKeyEnablesAuthentication) {
    ClientInfo client = makeClient("192.168.1.100");

    // Before adding key
    auto result1 = authService_->validatePublish("live", "new-key", client);
    EXPECT_FALSE(result1.allowed);

    // Add the key
    authService_->addStreamKey("new-key");

    // After adding key
    auto result2 = authService_->validatePublish("live", "new-key", client);
    EXPECT_TRUE(result2.allowed);
}

TEST_F(AuthServiceTest, RemoveStreamKeyRevokesAccess) {
    authService_->addStreamKey("temp-key");
    ClientInfo client = makeClient("192.168.1.100");

    // Before removal
    auto result1 = authService_->validatePublish("live", "temp-key", client);
    EXPECT_TRUE(result1.allowed);

    // Revoke the key
    authService_->removeStreamKey("temp-key");

    // After removal
    auto result2 = authService_->validatePublish("live", "temp-key", client);
    EXPECT_FALSE(result2.allowed);
}

TEST_F(AuthServiceTest, AddMultipleStreamKeys) {
    authService_->addStreamKey("key1");
    authService_->addStreamKey("key2");
    authService_->addStreamKey("key3");

    ClientInfo client = makeClient("192.168.1.100");

    EXPECT_TRUE(authService_->validatePublish("live", "key1", client).allowed);
    EXPECT_TRUE(authService_->validatePublish("live", "key2", client).allowed);
    EXPECT_TRUE(authService_->validatePublish("live", "key3", client).allowed);
    EXPECT_FALSE(authService_->validatePublish("live", "key4", client).allowed);
}

TEST_F(AuthServiceTest, GenerateStreamKeyCreatesValidKey) {
    std::string generatedKey = authService_->generateStreamKey();

    // Generated key should be non-empty
    EXPECT_FALSE(generatedKey.empty());

    // Generated key should be automatically added to allow list
    ClientInfo client = makeClient("192.168.1.100");
    auto result = authService_->validatePublish("live", generatedKey, client);
    EXPECT_TRUE(result.allowed);
}

TEST_F(AuthServiceTest, GenerateStreamKeyCreatesUniqueKeys) {
    std::set<std::string> keys;
    for (int i = 0; i < 100; ++i) {
        std::string key = authService_->generateStreamKey();
        EXPECT_TRUE(keys.insert(key).second) << "Duplicate key generated: " << key;
    }
}

TEST_F(AuthServiceTest, HasStreamKeyReturnsTrueForExisting) {
    authService_->addStreamKey("existing-key");

    EXPECT_TRUE(authService_->hasStreamKey("existing-key"));
    EXPECT_FALSE(authService_->hasStreamKey("nonexistent-key"));
}

TEST_F(AuthServiceTest, GetStreamKeyCountReturnsCorrectCount) {
    EXPECT_EQ(authService_->getStreamKeyCount(), 0u);

    authService_->addStreamKey("key1");
    EXPECT_EQ(authService_->getStreamKeyCount(), 1u);

    authService_->addStreamKey("key2");
    EXPECT_EQ(authService_->getStreamKeyCount(), 2u);

    authService_->removeStreamKey("key1");
    EXPECT_EQ(authService_->getStreamKeyCount(), 1u);
}

TEST_F(AuthServiceTest, ClearStreamKeyRemovesAll) {
    authService_->addStreamKey("key1");
    authService_->addStreamKey("key2");
    authService_->addStreamKey("key3");

    EXPECT_EQ(authService_->getStreamKeyCount(), 3u);

    authService_->clearStreamKeys();

    EXPECT_EQ(authService_->getStreamKeyCount(), 0u);
}

// =============================================================================
// Application-Scoped Keys Tests
// =============================================================================

TEST_F(AuthServiceTest, AddAppScopedStreamKey) {
    authService_->addStreamKey("app-key", "live");

    ClientInfo client = makeClient("192.168.1.100");

    // Valid for the specific app
    EXPECT_TRUE(authService_->validatePublish("live", "app-key", client).allowed);

    // Not valid for a different app
    EXPECT_FALSE(authService_->validatePublish("vod", "app-key", client).allowed);
}

TEST_F(AuthServiceTest, GlobalKeyWorksForAllApps) {
    // Key without app scope (global)
    authService_->addStreamKey("global-key");

    ClientInfo client = makeClient("192.168.1.100");

    // Valid for any app
    EXPECT_TRUE(authService_->validatePublish("live", "global-key", client).allowed);
    EXPECT_TRUE(authService_->validatePublish("vod", "global-key", client).allowed);
    EXPECT_TRUE(authService_->validatePublish("private", "global-key", client).allowed);
}

// =============================================================================
// Authentication Logging Tests (Requirement 15.5)
// =============================================================================

TEST_F(AuthServiceTest, AuthenticationAttemptsAreLogged) {
    std::vector<AuthLogEntry> logEntries;
    authService_->setAuthLogCallback([&logEntries](const AuthLogEntry& entry) {
        logEntries.push_back(entry);
    });

    authService_->addStreamKey("valid-key");
    ClientInfo client = makeClient("192.168.1.100", 54321);

    // Successful auth
    authService_->validatePublish("live", "valid-key", client);

    // Failed auth
    authService_->validatePublish("live", "invalid-key", client);

    ASSERT_EQ(logEntries.size(), 2u);

    // Check successful log entry
    EXPECT_EQ(logEntries[0].clientIP, "192.168.1.100");
    EXPECT_EQ(logEntries[0].streamKey, "valid-key");
    EXPECT_EQ(logEntries[0].app, "live");
    EXPECT_TRUE(logEntries[0].success);
    EXPECT_EQ(logEntries[0].action, AuthAction::Publish);

    // Check failed log entry
    EXPECT_EQ(logEntries[1].clientIP, "192.168.1.100");
    EXPECT_EQ(logEntries[1].streamKey, "invalid-key");
    EXPECT_FALSE(logEntries[1].success);
    EXPECT_TRUE(logEntries[1].reason.has_value());
}

TEST_F(AuthServiceTest, SubscribeAuthAttemptsAreLogged) {
    std::vector<AuthLogEntry> logEntries;
    authService_->setAuthLogCallback([&logEntries](const AuthLogEntry& entry) {
        logEntries.push_back(entry);
    });

    ClientInfo client = makeClient("10.0.0.1");

    // Subscribe with auth disabled (should succeed)
    authService_->validateSubscribe("live", "stream1", client);

    ASSERT_EQ(logEntries.size(), 1u);
    EXPECT_EQ(logEntries[0].action, AuthAction::Subscribe);
    EXPECT_TRUE(logEntries[0].success);
}

// =============================================================================
// Error Response Tests (Requirement 15.5)
// =============================================================================

TEST_F(AuthServiceTest, AuthFailureReturnsAppropriateReason) {
    authService_->addStreamKey("valid");
    ClientInfo client = makeClient("192.168.1.100");

    // Invalid key
    auto result1 = authService_->validatePublish("live", "invalid", client);
    EXPECT_FALSE(result1.allowed);
    EXPECT_TRUE(result1.reason.has_value());
    EXPECT_FALSE(result1.reason->empty());

    // Empty key
    auto result2 = authService_->validatePublish("live", "", client);
    EXPECT_FALSE(result2.allowed);
    EXPECT_TRUE(result2.reason.has_value());
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

class AuthServiceConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        AuthConfig config;
        config.publishAuthEnabled = true;
        authService_ = std::make_unique<AuthService>(config);
    }

    std::unique_ptr<AuthService> authService_;
};

TEST_F(AuthServiceConcurrencyTest, ConcurrentStreamKeyAdditionIsThreadSafe) {
    const int numThreads = 10;
    const int keysPerThread = 100;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, i, &successCount]() {
            for (int j = 0; j < 100; ++j) {
                std::string key = "key_" + std::to_string(i) + "_" + std::to_string(j);
                authService_->addStreamKey(key);
                successCount++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(successCount.load(), numThreads * keysPerThread);
    EXPECT_EQ(authService_->getStreamKeyCount(), static_cast<size_t>(numThreads * keysPerThread));
}

TEST_F(AuthServiceConcurrencyTest, ConcurrentValidationIsThreadSafe) {
    // Add some keys first
    for (int i = 0; i < 100; ++i) {
        authService_->addStreamKey("key_" + std::to_string(i));
    }

    const int numThreads = 10;
    const int validationsPerThread = 100;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};
    std::atomic<int> failureCount{0};

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, &successCount, &failureCount]() {
            ClientInfo client;
            client.ip = "192.168.1.1";
            client.port = 12345;

            for (int j = 0; j < 100; ++j) {
                std::string key = "key_" + std::to_string(j % 150);  // Some valid, some invalid
                auto result = authService_->validatePublish("live", key, client);
                if (result.allowed) {
                    successCount++;
                } else {
                    failureCount++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Verify we got the expected mix of success/failure
    int total = successCount.load() + failureCount.load();
    EXPECT_EQ(total, numThreads * validationsPerThread);
}

TEST_F(AuthServiceConcurrencyTest, ConcurrentAddRemoveIsThreadSafe) {
    std::vector<std::thread> threads;
    std::atomic<bool> running{true};

    // Add keys
    threads.emplace_back([this]() {
        for (int i = 0; i < 100; ++i) {
            authService_->addStreamKey("temp_key_" + std::to_string(i));
        }
    });

    // Remove keys
    threads.emplace_back([this]() {
        for (int i = 0; i < 100; ++i) {
            authService_->removeStreamKey("temp_key_" + std::to_string(i));
        }
    });

    // Validate keys
    threads.emplace_back([this, &running]() {
        ClientInfo client;
        client.ip = "192.168.1.1";
        while (running) {
            authService_->validatePublish("live", "temp_key_50", client);
            std::this_thread::yield();
        }
    });

    for (size_t i = 0; i < 2; ++i) {
        threads[i].join();
    }

    running = false;
    threads[2].join();

    // No crash or deadlock means success
    SUCCEED();
}

// =============================================================================
// Configuration Tests
// =============================================================================

TEST_F(AuthServiceTest, ConfigurationAffectsBehavior) {
    // Test with all auth disabled
    AuthConfig disabledConfig;
    disabledConfig.publishAuthEnabled = false;
    disabledConfig.subscribeAuthEnabled = false;
    auto disabledAuth = std::make_unique<AuthService>(disabledConfig);

    ClientInfo client = makeClient("192.168.1.100");

    // Both should allow without keys
    EXPECT_TRUE(disabledAuth->validatePublish("live", "any", client).allowed);
    EXPECT_TRUE(disabledAuth->validateSubscribe("live", "any", client).allowed);
}

TEST_F(AuthServiceTest, GetConfigurationReturnsCurrentConfig) {
    AuthConfig config;
    config.publishAuthEnabled = true;
    config.subscribeAuthEnabled = true;
    auto authService = std::make_unique<AuthService>(config);

    AuthConfig currentConfig = authService->getConfig();

    EXPECT_TRUE(currentConfig.publishAuthEnabled);
    EXPECT_TRUE(currentConfig.subscribeAuthEnabled);
}

TEST_F(AuthServiceTest, UpdateConfigurationChangeBehavior) {
    // Start with auth enabled
    AuthConfig config;
    config.publishAuthEnabled = true;
    auto authService = std::make_unique<AuthService>(config);

    authService->addStreamKey("key1");
    ClientInfo client = makeClient("192.168.1.100");

    // Should require valid key
    EXPECT_TRUE(authService->validatePublish("live", "key1", client).allowed);
    EXPECT_FALSE(authService->validatePublish("live", "invalid", client).allowed);

    // Disable auth
    AuthConfig newConfig;
    newConfig.publishAuthEnabled = false;
    authService->updateConfig(newConfig);

    // Now should allow any key
    EXPECT_TRUE(authService->validatePublish("live", "invalid", client).allowed);
}

// =============================================================================
// Command Processor Integration Interface Tests
// =============================================================================

TEST_F(AuthServiceTest, ProvideAuthResultForCommandProcessor) {
    authService_->addStreamKey("stream-key-abc");

    ClientInfo client = makeClient("192.168.1.100");

    // This simulates how command processor would use the auth service
    auto publishResult = authService_->validatePublish("live", "stream-key-abc", client);
    EXPECT_TRUE(publishResult.allowed);

    // Can be used to construct command error response
    auto failedResult = authService_->validatePublish("live", "wrong-key", client);
    EXPECT_FALSE(failedResult.allowed);
    EXPECT_TRUE(failedResult.reason.has_value());
    // Reason can be used in error response
    EXPECT_FALSE(failedResult.reason->empty());
}

} // namespace test
} // namespace core
} // namespace openrtmp
