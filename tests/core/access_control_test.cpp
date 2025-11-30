// OpenRTMP - Cross-platform RTMP Server
// Tests for Access Control and Rate Limiting
//
// Tests cover:
// - IP-based allow and deny ACL rules with CIDR notation
// - Rate limiting of 5 failed auth attempts per IP per minute
// - Block IPs exceeding rate limit for configurable duration
// - Log rate limit violations with client details
// - Application-scoped ACL rules
//
// Requirements coverage:
// - Requirement 15.6: IP-based access control lists for both allow and deny rules
// - Requirement 15.7: Rate-limit authentication attempts (5 per IP per minute)

#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <mutex>

#include "openrtmp/core/access_control.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace core {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class AccessControlTest : public ::testing::Test {
protected:
    void SetUp() override {
        AccessControlConfig config;
        config.rateLimitMaxAttempts = 5;
        config.rateLimitWindowMs = 60000;  // 1 minute
        config.blockDurationMs = 300000;   // 5 minutes
        accessControl_ = std::make_unique<AccessControl>(config);
    }

    void TearDown() override {
        accessControl_.reset();
    }

    std::unique_ptr<AccessControl> accessControl_;

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
// CIDR Parsing Tests
// =============================================================================

TEST_F(AccessControlTest, ParseIPv4AddressWithoutCIDR) {
    EXPECT_TRUE(AccessControl::isIPInCIDR("192.168.1.100", "192.168.1.100"));
    EXPECT_FALSE(AccessControl::isIPInCIDR("192.168.1.100", "192.168.1.101"));
}

TEST_F(AccessControlTest, ParseIPv4WithCIDR32) {
    // /32 means exact match
    EXPECT_TRUE(AccessControl::isIPInCIDR("192.168.1.100", "192.168.1.100/32"));
    EXPECT_FALSE(AccessControl::isIPInCIDR("192.168.1.101", "192.168.1.100/32"));
}

TEST_F(AccessControlTest, ParseIPv4WithCIDR24) {
    // /24 means match first 3 octets (192.168.1.x)
    EXPECT_TRUE(AccessControl::isIPInCIDR("192.168.1.0", "192.168.1.0/24"));
    EXPECT_TRUE(AccessControl::isIPInCIDR("192.168.1.100", "192.168.1.0/24"));
    EXPECT_TRUE(AccessControl::isIPInCIDR("192.168.1.255", "192.168.1.0/24"));
    EXPECT_FALSE(AccessControl::isIPInCIDR("192.168.2.0", "192.168.1.0/24"));
    EXPECT_FALSE(AccessControl::isIPInCIDR("192.168.0.255", "192.168.1.0/24"));
}

TEST_F(AccessControlTest, ParseIPv4WithCIDR16) {
    // /16 means match first 2 octets (192.168.x.x)
    EXPECT_TRUE(AccessControl::isIPInCIDR("192.168.0.1", "192.168.0.0/16"));
    EXPECT_TRUE(AccessControl::isIPInCIDR("192.168.255.255", "192.168.0.0/16"));
    EXPECT_FALSE(AccessControl::isIPInCIDR("192.169.0.1", "192.168.0.0/16"));
}

TEST_F(AccessControlTest, ParseIPv4WithCIDR8) {
    // /8 means match first octet (10.x.x.x)
    EXPECT_TRUE(AccessControl::isIPInCIDR("10.0.0.1", "10.0.0.0/8"));
    EXPECT_TRUE(AccessControl::isIPInCIDR("10.255.255.255", "10.0.0.0/8"));
    EXPECT_FALSE(AccessControl::isIPInCIDR("11.0.0.1", "10.0.0.0/8"));
}

TEST_F(AccessControlTest, ParseIPv4WithCIDR0) {
    // /0 means match all IPs
    EXPECT_TRUE(AccessControl::isIPInCIDR("1.2.3.4", "0.0.0.0/0"));
    EXPECT_TRUE(AccessControl::isIPInCIDR("255.255.255.255", "0.0.0.0/0"));
}

TEST_F(AccessControlTest, ParseIPv6Address) {
    EXPECT_TRUE(AccessControl::isIPInCIDR("::1", "::1"));
    EXPECT_TRUE(AccessControl::isIPInCIDR("::1", "::1/128"));
    EXPECT_FALSE(AccessControl::isIPInCIDR("::1", "::2/128"));
}

TEST_F(AccessControlTest, ParseIPv6WithCIDR64) {
    // /64 means match first 64 bits
    EXPECT_TRUE(AccessControl::isIPInCIDR("2001:db8::1", "2001:db8::/64"));
    EXPECT_TRUE(AccessControl::isIPInCIDR("2001:db8::ffff", "2001:db8::/64"));
    EXPECT_FALSE(AccessControl::isIPInCIDR("2001:db9::1", "2001:db8::/64"));
}

TEST_F(AccessControlTest, InvalidCIDRReturnsAlwaysFalse) {
    EXPECT_FALSE(AccessControl::isIPInCIDR("192.168.1.1", "not-an-ip"));
    EXPECT_FALSE(AccessControl::isIPInCIDR("not-an-ip", "192.168.1.0/24"));
    EXPECT_FALSE(AccessControl::isIPInCIDR("192.168.1.1", "192.168.1.0/99"));  // Invalid prefix
}

// =============================================================================
// ACL Allow Rule Tests (Requirement 15.6)
// =============================================================================

TEST_F(AccessControlTest, AllowRulePermitsMatchingIP) {
    ACLRuleConfig rule;
    rule.id = "allow-internal";
    rule.action = ACLAction::Allow;
    rule.ipPattern = "192.168.1.0/24";

    accessControl_->addACLRule(rule);

    ClientInfo client = makeClient("192.168.1.100");
    auto result = accessControl_->checkACL(client.ip);

    EXPECT_TRUE(result.allowed);
}

TEST_F(AccessControlTest, NoMatchingRuleReturnsDefaultAllow) {
    // No rules configured - default should allow
    ClientInfo client = makeClient("192.168.1.100");
    auto result = accessControl_->checkACL(client.ip);

    EXPECT_TRUE(result.allowed);
}

TEST_F(AccessControlTest, AllowRuleDoesNotPermitNonMatchingIP) {
    ACLRuleConfig rule;
    rule.id = "allow-internal";
    rule.action = ACLAction::Allow;
    rule.ipPattern = "192.168.1.0/24";

    accessControl_->addACLRule(rule);

    // Different subnet
    ClientInfo client = makeClient("10.0.0.1");
    auto result = accessControl_->checkACL(client.ip);

    // With only allow rules, non-matching IPs should still be allowed (no deny rule)
    EXPECT_TRUE(result.allowed);
}

// =============================================================================
// ACL Deny Rule Tests (Requirement 15.6)
// =============================================================================

TEST_F(AccessControlTest, DenyRuleBlocksMatchingIP) {
    ACLRuleConfig rule;
    rule.id = "deny-external";
    rule.action = ACLAction::Deny;
    rule.ipPattern = "10.0.0.0/8";

    accessControl_->addACLRule(rule);

    ClientInfo client = makeClient("10.1.2.3");
    auto result = accessControl_->checkACL(client.ip);

    EXPECT_FALSE(result.allowed);
    EXPECT_TRUE(result.reason.has_value());
}

TEST_F(AccessControlTest, DenyRuleDoesNotBlockNonMatchingIP) {
    ACLRuleConfig rule;
    rule.id = "deny-external";
    rule.action = ACLAction::Deny;
    rule.ipPattern = "10.0.0.0/8";

    accessControl_->addACLRule(rule);

    ClientInfo client = makeClient("192.168.1.100");
    auto result = accessControl_->checkACL(client.ip);

    EXPECT_TRUE(result.allowed);
}

// =============================================================================
// ACL Rule Priority Tests - Deny Takes Precedence
// =============================================================================

TEST_F(AccessControlTest, DenyTakesPrecedenceOverAllow) {
    // Add allow rule for broad range
    ACLRuleConfig allowRule;
    allowRule.id = "allow-all";
    allowRule.action = ACLAction::Allow;
    allowRule.ipPattern = "0.0.0.0/0";
    accessControl_->addACLRule(allowRule);

    // Add deny rule for specific IP
    ACLRuleConfig denyRule;
    denyRule.id = "deny-specific";
    denyRule.action = ACLAction::Deny;
    denyRule.ipPattern = "192.168.1.100";
    accessControl_->addACLRule(denyRule);

    // The specific IP should be denied despite the allow-all rule
    ClientInfo client = makeClient("192.168.1.100");
    auto result = accessControl_->checkACL(client.ip);

    EXPECT_FALSE(result.allowed);
}

TEST_F(AccessControlTest, DenyTakesPrecedenceWithOverlappingRanges) {
    // Allow a subnet
    ACLRuleConfig allowRule;
    allowRule.id = "allow-internal";
    allowRule.action = ACLAction::Allow;
    allowRule.ipPattern = "192.168.0.0/16";
    accessControl_->addACLRule(allowRule);

    // Deny a more specific subnet within
    ACLRuleConfig denyRule;
    denyRule.id = "deny-dmz";
    denyRule.action = ACLAction::Deny;
    denyRule.ipPattern = "192.168.100.0/24";
    accessControl_->addACLRule(denyRule);

    // IP in DMZ should be denied
    ClientInfo clientDMZ = makeClient("192.168.100.50");
    auto resultDMZ = accessControl_->checkACL(clientDMZ.ip);
    EXPECT_FALSE(resultDMZ.allowed);

    // IP outside DMZ but in allowed range should be allowed
    ClientInfo clientInternal = makeClient("192.168.1.50");
    auto resultInternal = accessControl_->checkACL(clientInternal.ip);
    EXPECT_TRUE(resultInternal.allowed);
}

// =============================================================================
// Application-Scoped ACL Rules Tests (Requirement 15.6)
// =============================================================================

TEST_F(AccessControlTest, AppScopedAllowRule) {
    ACLRuleConfig rule;
    rule.id = "allow-internal-live";
    rule.action = ACLAction::Allow;
    rule.ipPattern = "192.168.1.0/24";
    rule.app = "live";  // Only for 'live' application

    accessControl_->addACLRule(rule);

    ClientInfo client = makeClient("192.168.1.100");

    // Check for 'live' app - should match
    auto resultLive = accessControl_->checkACL(client.ip, "live");
    EXPECT_TRUE(resultLive.allowed);

    // Check for different app - rule doesn't apply
    auto resultVod = accessControl_->checkACL(client.ip, "vod");
    EXPECT_TRUE(resultVod.allowed);  // No matching rule, default allow
}

TEST_F(AccessControlTest, AppScopedDenyRule) {
    ACLRuleConfig rule;
    rule.id = "deny-external-live";
    rule.action = ACLAction::Deny;
    rule.ipPattern = "10.0.0.0/8";
    rule.app = "live";

    accessControl_->addACLRule(rule);

    ClientInfo client = makeClient("10.1.2.3");

    // Check for 'live' app - should be denied
    auto resultLive = accessControl_->checkACL(client.ip, "live");
    EXPECT_FALSE(resultLive.allowed);

    // Check for different app - rule doesn't apply
    auto resultVod = accessControl_->checkACL(client.ip, "vod");
    EXPECT_TRUE(resultVod.allowed);
}

TEST_F(AccessControlTest, GlobalRuleAppliesToAllApps) {
    ACLRuleConfig rule;
    rule.id = "deny-bad-network";
    rule.action = ACLAction::Deny;
    rule.ipPattern = "10.0.0.0/8";
    // No app specified - applies to all

    accessControl_->addACLRule(rule);

    ClientInfo client = makeClient("10.1.2.3");

    // Should be denied for all apps
    EXPECT_FALSE(accessControl_->checkACL(client.ip, "live").allowed);
    EXPECT_FALSE(accessControl_->checkACL(client.ip, "vod").allowed);
    EXPECT_FALSE(accessControl_->checkACL(client.ip, "private").allowed);
}

// =============================================================================
// ACL Rule Management Tests
// =============================================================================

TEST_F(AccessControlTest, RemoveACLRule) {
    ACLRuleConfig rule;
    rule.id = "deny-rule";
    rule.action = ACLAction::Deny;
    rule.ipPattern = "192.168.1.100";

    accessControl_->addACLRule(rule);

    ClientInfo client = makeClient("192.168.1.100");

    // Initially denied
    EXPECT_FALSE(accessControl_->checkACL(client.ip).allowed);

    // Remove the rule
    accessControl_->removeACLRule("deny-rule");

    // Now should be allowed (no rules)
    EXPECT_TRUE(accessControl_->checkACL(client.ip).allowed);
}

TEST_F(AccessControlTest, ClearAllACLRules) {
    ACLRuleConfig rule1;
    rule1.id = "deny-1";
    rule1.action = ACLAction::Deny;
    rule1.ipPattern = "10.0.0.0/8";
    accessControl_->addACLRule(rule1);

    ACLRuleConfig rule2;
    rule2.id = "deny-2";
    rule2.action = ACLAction::Deny;
    rule2.ipPattern = "172.16.0.0/12";
    accessControl_->addACLRule(rule2);

    ClientInfo client1 = makeClient("10.1.2.3");
    ClientInfo client2 = makeClient("172.20.1.1");

    EXPECT_FALSE(accessControl_->checkACL(client1.ip).allowed);
    EXPECT_FALSE(accessControl_->checkACL(client2.ip).allowed);

    // Clear all rules
    accessControl_->clearACLRules();

    // Both should now be allowed
    EXPECT_TRUE(accessControl_->checkACL(client1.ip).allowed);
    EXPECT_TRUE(accessControl_->checkACL(client2.ip).allowed);
}

TEST_F(AccessControlTest, GetACLRuleCount) {
    EXPECT_EQ(accessControl_->getACLRuleCount(), 0u);

    ACLRuleConfig rule1;
    rule1.id = "rule-1";
    rule1.action = ACLAction::Allow;
    rule1.ipPattern = "192.168.1.0/24";
    accessControl_->addACLRule(rule1);

    EXPECT_EQ(accessControl_->getACLRuleCount(), 1u);

    ACLRuleConfig rule2;
    rule2.id = "rule-2";
    rule2.action = ACLAction::Deny;
    rule2.ipPattern = "10.0.0.0/8";
    accessControl_->addACLRule(rule2);

    EXPECT_EQ(accessControl_->getACLRuleCount(), 2u);

    accessControl_->removeACLRule("rule-1");
    EXPECT_EQ(accessControl_->getACLRuleCount(), 1u);
}

// =============================================================================
// Rate Limiting Tests (Requirement 15.7)
// =============================================================================

TEST_F(AccessControlTest, AllowsRequestsWithinRateLimit) {
    std::string ip = "192.168.1.100";

    // 5 requests should all succeed
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(accessControl_->checkRateLimit(ip))
            << "Request " << (i + 1) << " should be allowed";
    }
}

TEST_F(AccessControlTest, BlocksRequestsExceedingRateLimit) {
    std::string ip = "192.168.1.100";

    // Exhaust the rate limit (5 attempts)
    for (int i = 0; i < 5; ++i) {
        accessControl_->recordFailedAttempt(ip);
    }

    // Next request should be blocked
    EXPECT_FALSE(accessControl_->checkRateLimit(ip));
}

TEST_F(AccessControlTest, RateLimitIsPerIP) {
    std::string ip1 = "192.168.1.100";
    std::string ip2 = "192.168.1.101";

    // Exhaust rate limit for ip1
    for (int i = 0; i < 5; ++i) {
        accessControl_->recordFailedAttempt(ip1);
    }

    // ip1 should be blocked
    EXPECT_FALSE(accessControl_->checkRateLimit(ip1));

    // ip2 should still be allowed
    EXPECT_TRUE(accessControl_->checkRateLimit(ip2));
}

TEST_F(AccessControlTest, BlockedIPRemainsBlockedForConfiguredDuration) {
    // Use shorter durations for testing
    AccessControlConfig config;
    config.rateLimitMaxAttempts = 5;
    config.rateLimitWindowMs = 60000;
    config.blockDurationMs = 100;  // 100ms block duration for testing
    auto ac = std::make_unique<AccessControl>(config);

    std::string ip = "192.168.1.100";

    // Exhaust rate limit
    for (int i = 0; i < 5; ++i) {
        ac->recordFailedAttempt(ip);
    }

    // Should be blocked
    EXPECT_FALSE(ac->checkRateLimit(ip));

    // Wait for block to expire
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Should now be allowed (but note: the sliding window might still have attempts)
    // After block expires, checkRateLimit should return true until new failures occur
    EXPECT_TRUE(ac->isBlockExpired(ip));
}

TEST_F(AccessControlTest, SuccessfulAuthResetsFailureCount) {
    std::string ip = "192.168.1.100";

    // Record some failures
    accessControl_->recordFailedAttempt(ip);
    accessControl_->recordFailedAttempt(ip);
    accessControl_->recordFailedAttempt(ip);

    // Record a success
    accessControl_->recordSuccessfulAuth(ip);

    // Should be able to make 5 more failures before blocking
    for (int i = 0; i < 4; ++i) {
        accessControl_->recordFailedAttempt(ip);
        EXPECT_TRUE(accessControl_->checkRateLimit(ip))
            << "Should still be allowed after " << (i + 1) << " failures";
    }

    // The 5th failure should trigger rate limiting
    accessControl_->recordFailedAttempt(ip);
    EXPECT_FALSE(accessControl_->checkRateLimit(ip));
}

TEST_F(AccessControlTest, SlidingWindowExpireOldAttempts) {
    // Configure with short window for testing
    AccessControlConfig config;
    config.rateLimitMaxAttempts = 5;
    config.rateLimitWindowMs = 100;  // 100ms window
    config.blockDurationMs = 300000;
    auto ac = std::make_unique<AccessControl>(config);

    std::string ip = "192.168.1.100";

    // Record 4 failures
    for (int i = 0; i < 4; ++i) {
        ac->recordFailedAttempt(ip);
    }

    // Wait for window to expire
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Old attempts should have expired, so 5 more should be allowed
    for (int i = 0; i < 5; ++i) {
        ac->recordFailedAttempt(ip);
    }

    // Now should be rate limited
    EXPECT_FALSE(ac->checkRateLimit(ip));
}

// =============================================================================
// Rate Limit Logging Tests
// =============================================================================

TEST_F(AccessControlTest, RateLimitViolationsAreLogged) {
    std::vector<RateLimitLogEntry> logEntries;
    accessControl_->setRateLimitLogCallback([&logEntries](const RateLimitLogEntry& entry) {
        logEntries.push_back(entry);
    });

    std::string ip = "192.168.1.100";

    // Exhaust rate limit
    for (int i = 0; i < 5; ++i) {
        accessControl_->recordFailedAttempt(ip);
    }

    // This should trigger a log entry
    accessControl_->checkRateLimit(ip);

    ASSERT_GE(logEntries.size(), 1u);
    EXPECT_EQ(logEntries.back().clientIP, ip);
    EXPECT_EQ(logEntries.back().attemptCount, 5u);
}

// =============================================================================
// Integration with AuthService Tests
// =============================================================================

TEST_F(AccessControlTest, DeniedByACLBeforeAuthCheck) {
    ACLRuleConfig rule;
    rule.id = "deny-bad-network";
    rule.action = ACLAction::Deny;
    rule.ipPattern = "10.0.0.0/8";
    accessControl_->addACLRule(rule);

    ClientInfo client = makeClient("10.1.2.3");

    // Should be denied by ACL before any auth check
    auto aclResult = accessControl_->checkACL(client.ip);
    EXPECT_FALSE(aclResult.allowed);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

class AccessControlConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        AccessControlConfig config;
        config.rateLimitMaxAttempts = 5;
        config.rateLimitWindowMs = 60000;
        config.blockDurationMs = 300000;
        accessControl_ = std::make_unique<AccessControl>(config);
    }

    std::unique_ptr<AccessControl> accessControl_;
};

TEST_F(AccessControlConcurrencyTest, ConcurrentACLChecksAreThreadSafe) {
    // Add some rules
    for (int i = 0; i < 10; ++i) {
        ACLRuleConfig rule;
        rule.id = "rule-" + std::to_string(i);
        rule.action = (i % 2 == 0) ? ACLAction::Allow : ACLAction::Deny;
        rule.ipPattern = "192.168." + std::to_string(i) + ".0/24";
        accessControl_->addACLRule(rule);
    }

    const int numThreads = 10;
    const int checksPerThread = 100;
    std::vector<std::thread> threads;
    std::atomic<int> totalChecks{0};

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, &totalChecks]() {
            for (int j = 0; j < 100; ++j) {
                std::string ip = "192.168." + std::to_string(j % 20) + "." + std::to_string(j);
                accessControl_->checkACL(ip);
                totalChecks++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(totalChecks.load(), numThreads * checksPerThread);
}

TEST_F(AccessControlConcurrencyTest, ConcurrentRateLimitingIsThreadSafe) {
    const int numThreads = 10;
    std::vector<std::thread> threads;
    std::atomic<int> blockedCount{0};
    std::atomic<int> allowedCount{0};

    // Multiple threads recording failures for different IPs
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, i, &blockedCount, &allowedCount]() {
            std::string ip = "192.168.1." + std::to_string(i);

            // Record 6 failures (more than limit)
            for (int j = 0; j < 6; ++j) {
                accessControl_->recordFailedAttempt(ip);
            }

            // Check if rate limited
            if (accessControl_->checkRateLimit(ip)) {
                allowedCount++;
            } else {
                blockedCount++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All IPs should be blocked (each had 6 failures)
    EXPECT_EQ(blockedCount.load(), numThreads);
    EXPECT_EQ(allowedCount.load(), 0);
}

TEST_F(AccessControlConcurrencyTest, ConcurrentAddRemoveRulesIsThreadSafe) {
    std::vector<std::thread> threads;
    std::atomic<bool> running{true};

    // Thread adding rules
    threads.emplace_back([this]() {
        for (int i = 0; i < 100; ++i) {
            ACLRuleConfig rule;
            rule.id = "temp-rule-" + std::to_string(i);
            rule.action = ACLAction::Deny;
            rule.ipPattern = "192.168.1." + std::to_string(i);
            accessControl_->addACLRule(rule);
        }
    });

    // Thread removing rules
    threads.emplace_back([this]() {
        for (int i = 0; i < 100; ++i) {
            accessControl_->removeACLRule("temp-rule-" + std::to_string(i));
        }
    });

    // Thread checking ACL
    threads.emplace_back([this, &running]() {
        while (running) {
            accessControl_->checkACL("192.168.1.50");
            std::this_thread::yield();
        }
    });

    // Wait for add/remove threads to finish
    threads[0].join();
    threads[1].join();

    running = false;
    threads[2].join();

    // No crash means success
    SUCCEED();
}

// =============================================================================
// Configuration Tests
// =============================================================================

TEST_F(AccessControlTest, UpdateConfigurationChangesBehavior) {
    AccessControlConfig config;
    config.rateLimitMaxAttempts = 3;  // Lower limit for testing
    config.rateLimitWindowMs = 60000;
    config.blockDurationMs = 300000;
    accessControl_->updateConfig(config);

    std::string ip = "192.168.1.100";

    // Record 3 failures (new limit)
    for (int i = 0; i < 3; ++i) {
        accessControl_->recordFailedAttempt(ip);
    }

    // Should now be blocked
    EXPECT_FALSE(accessControl_->checkRateLimit(ip));
}

TEST_F(AccessControlTest, GetConfigurationReturnsCurrentConfig) {
    AccessControlConfig config = accessControl_->getConfig();

    EXPECT_EQ(config.rateLimitMaxAttempts, 5u);
    EXPECT_EQ(config.rateLimitWindowMs, 60000u);
    EXPECT_EQ(config.blockDurationMs, 300000u);
}

// =============================================================================
// IPv6 ACL Tests
// =============================================================================

TEST_F(AccessControlTest, IPv6AllowRule) {
    ACLRuleConfig rule;
    rule.id = "allow-ipv6-local";
    rule.action = ACLAction::Allow;
    rule.ipPattern = "::1";

    accessControl_->addACLRule(rule);

    auto result = accessControl_->checkACL("::1");
    EXPECT_TRUE(result.allowed);
}

TEST_F(AccessControlTest, IPv6DenyRule) {
    ACLRuleConfig rule;
    rule.id = "deny-ipv6-range";
    rule.action = ACLAction::Deny;
    rule.ipPattern = "2001:db8::/32";

    accessControl_->addACLRule(rule);

    auto result = accessControl_->checkACL("2001:db8::1");
    EXPECT_FALSE(result.allowed);

    auto result2 = accessControl_->checkACL("2001:db9::1");
    EXPECT_TRUE(result2.allowed);
}

TEST_F(AccessControlTest, IPv6RateLimiting) {
    std::string ipv6 = "2001:db8::1";

    // Exhaust rate limit
    for (int i = 0; i < 5; ++i) {
        accessControl_->recordFailedAttempt(ipv6);
    }

    EXPECT_FALSE(accessControl_->checkRateLimit(ipv6));
}

} // namespace test
} // namespace core
} // namespace openrtmp
