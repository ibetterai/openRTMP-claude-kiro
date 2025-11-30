// OpenRTMP - Cross-platform RTMP Server
// Access Control and Rate Limiting Implementation
//
// Thread-safe implementation with:
// - IP-based ACL rules with CIDR notation (IPv4 and IPv6)
// - Deny rules take precedence over allow rules
// - Application-scoped rules
// - Sliding window rate limiting
// - Configurable block duration
// - Rate limit violation logging
//
// Requirements coverage:
// - Requirement 15.6: IP-based access control lists for both allow and deny rules
// - Requirement 15.7: Rate-limit authentication attempts (5 per IP per minute)

#include "openrtmp/core/access_control.hpp"

#include <algorithm>
#include <sstream>
#include <cstring>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
#endif

namespace openrtmp {
namespace core {

// =============================================================================
// Constructor / Destructor
// =============================================================================

AccessControl::AccessControl(const AccessControlConfig& config)
    : config_(config)
{
}

AccessControl::~AccessControl() = default;

AccessControl::AccessControl(AccessControl&& other) noexcept
    : config_(std::move(other.config_))
    , aclRules_(std::move(other.aclRules_))
    , rateLimitState_(std::move(other.rateLimitState_))
{
    std::lock_guard<std::mutex> lock(other.logCallbackMutex_);
    logCallback_ = std::move(other.logCallback_);
}

AccessControl& AccessControl::operator=(AccessControl&& other) noexcept {
    if (this != &other) {
        std::unique_lock<std::shared_mutex> configLock1(configMutex_, std::defer_lock);
        std::unique_lock<std::shared_mutex> aclLock1(aclMutex_, std::defer_lock);
        std::unique_lock<std::shared_mutex> configLock2(other.configMutex_, std::defer_lock);
        std::unique_lock<std::shared_mutex> aclLock2(other.aclMutex_, std::defer_lock);

        std::lock(configLock1, aclLock1, configLock2, aclLock2);

        config_ = std::move(other.config_);
        aclRules_ = std::move(other.aclRules_);

        {
            std::lock_guard<std::mutex> rateLock1(rateLimitMutex_);
            std::lock_guard<std::mutex> rateLock2(other.rateLimitMutex_);
            rateLimitState_ = std::move(other.rateLimitState_);
        }

        {
            std::lock_guard<std::mutex> logLock1(logCallbackMutex_);
            std::lock_guard<std::mutex> logLock2(other.logCallbackMutex_);
            logCallback_ = std::move(other.logCallback_);
        }
    }
    return *this;
}

// =============================================================================
// Static CIDR Utilities
// =============================================================================

bool IAccessControl::parseIP(const std::string& ip, bool& isIPv6, std::array<uint8_t, 16>& bytes) {
    bytes.fill(0);

    // Try IPv4 first
    struct in_addr addr4;
    if (inet_pton(AF_INET, ip.c_str(), &addr4) == 1) {
        isIPv6 = false;
        std::memcpy(bytes.data(), &addr4.s_addr, 4);
        return true;
    }

    // Try IPv6
    struct in6_addr addr6;
    if (inet_pton(AF_INET6, ip.c_str(), &addr6) == 1) {
        isIPv6 = true;
        std::memcpy(bytes.data(), &addr6.s6_addr, 16);
        return true;
    }

    return false;
}

ParsedCIDR IAccessControl::parseCIDR(const std::string& cidr) {
    ParsedCIDR result;
    result.valid = false;

    if (cidr.empty()) {
        return result;
    }

    // Find the '/' separator
    size_t slashPos = cidr.find('/');
    std::string ipPart;
    int prefixLen = -1;

    if (slashPos != std::string::npos) {
        ipPart = cidr.substr(0, slashPos);
        std::string prefixStr = cidr.substr(slashPos + 1);

        // Parse prefix length
        try {
            prefixLen = std::stoi(prefixStr);
        } catch (...) {
            return result;  // Invalid prefix
        }
    } else {
        ipPart = cidr;
    }

    // Parse the IP address
    if (!parseIP(ipPart, result.isIPv6, result.address)) {
        return result;
    }

    // Set default prefix length if not specified
    if (prefixLen < 0) {
        prefixLen = result.isIPv6 ? 128 : 32;
    }

    // Validate prefix length
    int maxPrefix = result.isIPv6 ? 128 : 32;
    if (prefixLen < 0 || prefixLen > maxPrefix) {
        return result;
    }

    result.prefixLength = static_cast<uint8_t>(prefixLen);
    result.valid = true;

    return result;
}

bool IAccessControl::isIPInCIDR(const std::string& ip, const std::string& cidr) {
    // Parse the CIDR
    ParsedCIDR parsedCIDR = parseCIDR(cidr);
    if (!parsedCIDR.valid) {
        return false;
    }

    // Parse the IP address
    bool isIPv6;
    std::array<uint8_t, 16> ipBytes;
    if (!parseIP(ip, isIPv6, ipBytes)) {
        return false;
    }

    // Check if IP types match
    if (isIPv6 != parsedCIDR.isIPv6) {
        return false;
    }

    // Get the number of bytes to compare
    size_t totalBytes = isIPv6 ? 16u : 4u;
    size_t prefixLen = parsedCIDR.prefixLength;

    // Special case: /0 matches everything
    if (prefixLen == 0) {
        return true;
    }

    // Compare full bytes
    size_t fullBytes = prefixLen / 8;
    for (size_t i = 0; i < fullBytes; ++i) {
        if (ipBytes[i] != parsedCIDR.address[i]) {
            return false;
        }
    }

    // Compare remaining bits
    size_t remainingBits = prefixLen % 8;
    if (remainingBits > 0 && fullBytes < totalBytes) {
        uint8_t mask = static_cast<uint8_t>(0xFF << (8 - remainingBits));
        if ((ipBytes[fullBytes] & mask) != (parsedCIDR.address[fullBytes] & mask)) {
            return false;
        }
    }

    return true;
}

// =============================================================================
// ACL Operations
// =============================================================================

ACLCheckResult AccessControl::checkACL(
    const std::string& ip,
    const std::string& app
) {
    std::shared_lock<std::shared_mutex> lock(aclMutex_);

    // If no rules configured, allow by default
    if (aclRules_.empty()) {
        return ACLCheckResult::allow();
    }

    // Check all rules - deny takes precedence
    bool hasMatchingAllowRule = false;
    std::string matchingAllowRuleId;

    for (const auto& [ruleId, rule] : aclRules_) {
        // Check if rule applies to this app
        if (rule.config.app.has_value() && !rule.config.app->empty()) {
            if (app.empty() || rule.config.app.value() != app) {
                continue;  // Rule doesn't apply to this app
            }
        }

        // Check if IP matches the rule's CIDR
        if (!rule.parsedCIDR.valid) {
            continue;
        }

        if (matchesCIDR(ip, rule.parsedCIDR)) {
            if (rule.config.action == ACLAction::Deny) {
                // Deny takes precedence - return immediately
                return ACLCheckResult::deny("IP denied by ACL rule: " + ruleId, ruleId);
            } else {
                // Remember that we found a matching allow rule
                hasMatchingAllowRule = true;
                matchingAllowRuleId = ruleId;
            }
        }
    }

    // If we found a matching allow rule and no deny rule matched, allow
    if (hasMatchingAllowRule) {
        ACLCheckResult result;
        result.allowed = true;
        result.matchedRule = matchingAllowRuleId;
        return result;
    }

    // No matching rules - allow by default
    return ACLCheckResult::allow();
}

bool AccessControl::matchesCIDR(const std::string& ip, const ParsedCIDR& cidr) const {
    // Parse the IP address
    bool isIPv6;
    std::array<uint8_t, 16> ipBytes;
    if (!IAccessControl::parseIP(ip, isIPv6, ipBytes)) {
        return false;
    }

    // Check if IP types match
    if (isIPv6 != cidr.isIPv6) {
        return false;
    }

    // Get the number of bytes to compare
    size_t totalBytes = isIPv6 ? 16u : 4u;
    size_t prefixLen = cidr.prefixLength;

    // Special case: /0 matches everything
    if (prefixLen == 0) {
        return true;
    }

    // Compare full bytes
    size_t fullBytes = prefixLen / 8;
    for (size_t i = 0; i < fullBytes; ++i) {
        if (ipBytes[i] != cidr.address[i]) {
            return false;
        }
    }

    // Compare remaining bits
    size_t remainingBits = prefixLen % 8;
    if (remainingBits > 0 && fullBytes < totalBytes) {
        uint8_t mask = static_cast<uint8_t>(0xFF << (8 - remainingBits));
        if ((ipBytes[fullBytes] & mask) != (cidr.address[fullBytes] & mask)) {
            return false;
        }
    }

    return true;
}

void AccessControl::addACLRule(const ACLRuleConfig& rule) {
    std::unique_lock<std::shared_mutex> lock(aclMutex_);

    ParsedACLRule parsedRule;
    parsedRule.config = rule;
    parsedRule.parsedCIDR = IAccessControl::parseCIDR(rule.ipPattern);

    aclRules_[rule.id] = std::move(parsedRule);
}

void AccessControl::removeACLRule(const std::string& ruleId) {
    std::unique_lock<std::shared_mutex> lock(aclMutex_);
    aclRules_.erase(ruleId);
}

void AccessControl::clearACLRules() {
    std::unique_lock<std::shared_mutex> lock(aclMutex_);
    aclRules_.clear();
}

size_t AccessControl::getACLRuleCount() const {
    std::shared_lock<std::shared_mutex> lock(aclMutex_);
    return aclRules_.size();
}

// =============================================================================
// Rate Limiting Operations
// =============================================================================

bool AccessControl::checkRateLimit(const std::string& clientIP) {
    std::lock_guard<std::mutex> lock(rateLimitMutex_);

    auto it = rateLimitState_.find(clientIP);
    if (it == rateLimitState_.end()) {
        return true;  // No state, not rate limited
    }

    RateLimitState& state = it->second;

    // Check if currently blocked
    if (state.blockedUntil.has_value()) {
        auto now = std::chrono::steady_clock::now();
        if (now < state.blockedUntil.value()) {
            // Still blocked - log the violation
            logRateLimitViolation(clientIP, static_cast<uint32_t>(state.failedAttempts.size()));
            return false;
        } else {
            // Block expired, clear the state
            state.blockedUntil = std::nullopt;
            state.failedAttempts.clear();
            return true;
        }
    }

    // Clean up expired attempts
    cleanupExpiredAttempts(state);

    // Get config values
    uint32_t maxAttempts;
    {
        std::shared_lock<std::shared_mutex> configLock(configMutex_);
        maxAttempts = config_.rateLimitMaxAttempts;
    }

    // Check if rate limited
    if (state.failedAttempts.size() >= maxAttempts) {
        // Log the violation
        logRateLimitViolation(clientIP, static_cast<uint32_t>(state.failedAttempts.size()));
        return false;
    }

    return true;
}

void AccessControl::recordFailedAttempt(const std::string& clientIP) {
    std::lock_guard<std::mutex> lock(rateLimitMutex_);

    RateLimitState& state = rateLimitState_[clientIP];

    // Clean up expired attempts first
    cleanupExpiredAttempts(state);

    // Record the new attempt
    state.failedAttempts.push_back(std::chrono::steady_clock::now());

    // Check if we should block
    uint32_t maxAttempts;
    uint32_t blockDurationMs;
    {
        std::shared_lock<std::shared_mutex> configLock(configMutex_);
        maxAttempts = config_.rateLimitMaxAttempts;
        blockDurationMs = config_.blockDurationMs;
    }

    if (state.failedAttempts.size() >= maxAttempts) {
        // Set block time
        state.blockedUntil = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(blockDurationMs);
    }
}

void AccessControl::recordSuccessfulAuth(const std::string& clientIP) {
    std::lock_guard<std::mutex> lock(rateLimitMutex_);

    auto it = rateLimitState_.find(clientIP);
    if (it != rateLimitState_.end()) {
        // Clear failed attempts on success
        it->second.failedAttempts.clear();
        it->second.blockedUntil = std::nullopt;
    }
}

bool AccessControl::isBlockExpired(const std::string& clientIP) {
    std::lock_guard<std::mutex> lock(rateLimitMutex_);

    auto it = rateLimitState_.find(clientIP);
    if (it == rateLimitState_.end()) {
        return true;  // No state means no block
    }

    const RateLimitState& state = it->second;

    if (!state.blockedUntil.has_value()) {
        return true;  // Not blocked
    }

    auto now = std::chrono::steady_clock::now();
    return now >= state.blockedUntil.value();
}

void AccessControl::cleanupExpiredAttempts(RateLimitState& state) {
    uint32_t windowMs;
    {
        std::shared_lock<std::shared_mutex> configLock(configMutex_);
        windowMs = config_.rateLimitWindowMs;
    }

    auto now = std::chrono::steady_clock::now();
    auto windowStart = now - std::chrono::milliseconds(windowMs);

    // Remove attempts older than the window
    state.failedAttempts.erase(
        std::remove_if(state.failedAttempts.begin(), state.failedAttempts.end(),
            [windowStart](const auto& t) { return t < windowStart; }),
        state.failedAttempts.end()
    );
}

void AccessControl::logRateLimitViolation(const std::string& clientIP, uint32_t attemptCount) {
    RateLimitLogCallback callback;
    {
        std::lock_guard<std::mutex> lock(logCallbackMutex_);
        callback = logCallback_;
    }

    if (callback) {
        RateLimitLogEntry entry;
        entry.clientIP = clientIP;
        entry.attemptCount = attemptCount;
        entry.timestamp = std::chrono::steady_clock::now();

        uint32_t blockDurationMs;
        {
            std::shared_lock<std::shared_mutex> configLock(configMutex_);
            blockDurationMs = config_.blockDurationMs;
        }
        entry.blockDuration = std::chrono::milliseconds(blockDurationMs);

        callback(entry);
    }
}

// =============================================================================
// Configuration
// =============================================================================

AccessControlConfig AccessControl::getConfig() const {
    std::shared_lock<std::shared_mutex> lock(configMutex_);
    return config_;
}

void AccessControl::updateConfig(const AccessControlConfig& config) {
    std::unique_lock<std::shared_mutex> lock(configMutex_);
    config_ = config;
}

// =============================================================================
// Logging
// =============================================================================

void AccessControl::setRateLimitLogCallback(RateLimitLogCallback callback) {
    std::lock_guard<std::mutex> lock(logCallbackMutex_);
    logCallback_ = std::move(callback);
}

} // namespace core
} // namespace openrtmp
