// OpenRTMP - Cross-platform RTMP Server
// Access Control and Rate Limiting - IP-based ACL and auth rate limiting
//
// Responsibilities:
// - Support IP-based allow and deny ACL rules with CIDR notation
// - Enforce rate limiting of 5 failed auth attempts per IP per minute
// - Block IPs exceeding rate limit for configurable duration
// - Log rate limit violations with client details
// - Support application-scoped ACL rules
//
// Requirements coverage:
// - Requirement 15.6: IP-based access control lists for both allow and deny rules
// - Requirement 15.7: Rate-limit authentication attempts (5 per IP per minute)

#ifndef OPENRTMP_CORE_ACCESS_CONTROL_HPP
#define OPENRTMP_CORE_ACCESS_CONTROL_HPP

#include <cstdint>
#include <string>
#include <optional>
#include <memory>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <array>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/result.hpp"
#include "openrtmp/core/auth_service.hpp"

namespace openrtmp {
namespace core {

// =============================================================================
// Access Control Configuration
// =============================================================================

/**
 * @brief Configuration for access control service.
 */
struct AccessControlConfig {
    uint32_t rateLimitMaxAttempts = 5;      ///< Max failed attempts before rate limiting (per minute)
    uint32_t rateLimitWindowMs = 60000;     ///< Sliding window duration in milliseconds (1 minute)
    uint32_t blockDurationMs = 300000;      ///< Block duration in milliseconds (5 minutes)
    bool logViolations = true;              ///< Log rate limit violations
};

// =============================================================================
// ACL Rule Configuration
// =============================================================================

/**
 * @brief Extended ACL rule configuration with CIDR support.
 */
struct ACLRuleConfig {
    std::string id;                         ///< Unique rule identifier
    ACLAction action;                       ///< Allow or deny
    std::string ipPattern;                  ///< IP pattern with CIDR notation (e.g., "192.168.1.0/24")
    std::optional<std::string> app;         ///< Optional app scope (empty for global rules)
    int priority = 0;                       ///< Rule priority (higher = evaluated first, deny always wins)

    ACLRuleConfig() : action(ACLAction::Allow) {}
    ACLRuleConfig(std::string ruleId, ACLAction act, std::string pattern)
        : id(std::move(ruleId))
        , action(act)
        , ipPattern(std::move(pattern))
    {}
};

// =============================================================================
// ACL Check Result
// =============================================================================

/**
 * @brief Result of an ACL check operation.
 */
struct ACLCheckResult {
    bool allowed;                           ///< Whether the IP is allowed
    std::optional<std::string> reason;      ///< Reason for denial (if not allowed)
    std::optional<std::string> matchedRule; ///< ID of the rule that matched

    /**
     * @brief Create an allowed result.
     */
    static ACLCheckResult allow() {
        return {true, std::nullopt, std::nullopt};
    }

    /**
     * @brief Create a denied result with reason.
     */
    static ACLCheckResult deny(std::string reasonMsg, std::string ruleId = "") {
        return {false, std::move(reasonMsg), ruleId.empty() ? std::nullopt : std::optional<std::string>(std::move(ruleId))};
    }
};

// =============================================================================
// Rate Limit Log Entry
// =============================================================================

/**
 * @brief Log entry for rate limit violations.
 */
struct RateLimitLogEntry {
    std::string clientIP;                   ///< Client IP address that triggered the limit
    uint32_t attemptCount;                  ///< Number of failed attempts
    std::chrono::steady_clock::time_point timestamp;  ///< Timestamp of violation
    std::chrono::milliseconds blockDuration;///< Duration of the block

    RateLimitLogEntry()
        : attemptCount(0)
        , timestamp(std::chrono::steady_clock::now())
        , blockDuration(0)
    {}
};

/**
 * @brief Callback type for rate limit log events.
 */
using RateLimitLogCallback = std::function<void(const RateLimitLogEntry&)>;

// =============================================================================
// CIDR Helper Structure
// =============================================================================

/**
 * @brief Parsed CIDR address for efficient matching.
 */
struct ParsedCIDR {
    bool isIPv6 = false;                    ///< True if IPv6, false if IPv4
    std::array<uint8_t, 16> address{};      ///< IP address bytes (4 for IPv4, 16 for IPv6)
    uint8_t prefixLength = 0;               ///< CIDR prefix length
    bool valid = false;                     ///< Whether parsing succeeded
};

// =============================================================================
// Access Control Interface
// =============================================================================

/**
 * @brief Interface for access control service operations.
 *
 * Defines the contract for IP-based access control and rate limiting.
 */
class IAccessControl {
public:
    virtual ~IAccessControl() = default;

    // -------------------------------------------------------------------------
    // ACL Operations
    // -------------------------------------------------------------------------

    /**
     * @brief Check if an IP address is allowed by ACL rules.
     *
     * @param ip IP address to check
     * @param app Optional application name for app-scoped rules
     * @return ACLCheckResult indicating if access is allowed
     */
    virtual ACLCheckResult checkACL(
        const std::string& ip,
        const std::string& app = ""
    ) = 0;

    /**
     * @brief Add an ACL rule.
     *
     * @param rule ACL rule configuration to add
     */
    virtual void addACLRule(const ACLRuleConfig& rule) = 0;

    /**
     * @brief Remove an ACL rule by ID.
     *
     * @param ruleId Rule ID to remove
     */
    virtual void removeACLRule(const std::string& ruleId) = 0;

    /**
     * @brief Clear all ACL rules.
     */
    virtual void clearACLRules() = 0;

    /**
     * @brief Get the number of ACL rules.
     *
     * @return Number of configured ACL rules
     */
    virtual size_t getACLRuleCount() const = 0;

    // -------------------------------------------------------------------------
    // Rate Limiting Operations
    // -------------------------------------------------------------------------

    /**
     * @brief Check if a client IP is within rate limits.
     *
     * @param clientIP Client IP to check
     * @return true if within limits, false if rate limited
     */
    virtual bool checkRateLimit(const std::string& clientIP) = 0;

    /**
     * @brief Record a failed authentication attempt.
     *
     * @param clientIP Client IP that failed auth
     */
    virtual void recordFailedAttempt(const std::string& clientIP) = 0;

    /**
     * @brief Record a successful authentication to reset failure count.
     *
     * @param clientIP Client IP that succeeded auth
     */
    virtual void recordSuccessfulAuth(const std::string& clientIP) = 0;

    /**
     * @brief Check if a block has expired for an IP.
     *
     * @param clientIP Client IP to check
     * @return true if block has expired or no block exists
     */
    virtual bool isBlockExpired(const std::string& clientIP) = 0;

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    /**
     * @brief Get current configuration.
     *
     * @return Current access control configuration
     */
    virtual AccessControlConfig getConfig() const = 0;

    /**
     * @brief Update configuration.
     *
     * @param config New configuration
     */
    virtual void updateConfig(const AccessControlConfig& config) = 0;

    // -------------------------------------------------------------------------
    // Logging
    // -------------------------------------------------------------------------

    /**
     * @brief Set the rate limit violation log callback.
     *
     * @param callback Callback to invoke on rate limit violations
     */
    virtual void setRateLimitLogCallback(RateLimitLogCallback callback) = 0;

    // -------------------------------------------------------------------------
    // Static Utilities
    // -------------------------------------------------------------------------

    /**
     * @brief Check if an IP address matches a CIDR pattern.
     *
     * @param ip IP address to check (IPv4 or IPv6)
     * @param cidr CIDR pattern (e.g., "192.168.1.0/24" or "2001:db8::/32")
     * @return true if IP matches the CIDR pattern
     */
    static bool isIPInCIDR(const std::string& ip, const std::string& cidr);

    /**
     * @brief Parse a CIDR string into a structured format.
     *
     * @param cidr CIDR string to parse
     * @return ParsedCIDR structure
     */
    static ParsedCIDR parseCIDR(const std::string& cidr);

    /**
     * @brief Parse an IP address into bytes.
     *
     * @param ip IP address string
     * @param isIPv6 Output: true if IPv6
     * @param bytes Output: IP address bytes
     * @return true if parsing succeeded
     */
    static bool parseIP(const std::string& ip, bool& isIPv6, std::array<uint8_t, 16>& bytes);
};

// =============================================================================
// Access Control Implementation
// =============================================================================

/**
 * @brief Thread-safe access control implementation.
 *
 * Implements the IAccessControl interface with:
 * - IP-based ACL rules with CIDR notation
 * - Deny rules take precedence over allow rules
 * - Application-scoped rules
 * - Sliding window rate limiting
 * - Configurable block duration
 * - Rate limit violation logging
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses std::shared_mutex for read/write locking
 */
class AccessControl : public IAccessControl {
public:
    /**
     * @brief Construct an AccessControl with configuration.
     *
     * @param config Access control configuration
     */
    explicit AccessControl(const AccessControlConfig& config = AccessControlConfig());

    /**
     * @brief Destructor.
     */
    ~AccessControl() override;

    // Non-copyable
    AccessControl(const AccessControl&) = delete;
    AccessControl& operator=(const AccessControl&) = delete;

    // Movable
    AccessControl(AccessControl&&) noexcept;
    AccessControl& operator=(AccessControl&&) noexcept;

    // IAccessControl interface implementation
    ACLCheckResult checkACL(
        const std::string& ip,
        const std::string& app = ""
    ) override;

    void addACLRule(const ACLRuleConfig& rule) override;

    void removeACLRule(const std::string& ruleId) override;

    void clearACLRules() override;

    size_t getACLRuleCount() const override;

    bool checkRateLimit(const std::string& clientIP) override;

    void recordFailedAttempt(const std::string& clientIP) override;

    void recordSuccessfulAuth(const std::string& clientIP) override;

    bool isBlockExpired(const std::string& clientIP) override;

    AccessControlConfig getConfig() const override;

    void updateConfig(const AccessControlConfig& config) override;

    void setRateLimitLogCallback(RateLimitLogCallback callback) override;

private:
    /**
     * @brief Internal structure for tracking rate limit state per IP.
     */
    struct RateLimitState {
        std::vector<std::chrono::steady_clock::time_point> failedAttempts;
        std::optional<std::chrono::steady_clock::time_point> blockedUntil;
    };

    /**
     * @brief Internal structure for parsed ACL rule.
     */
    struct ParsedACLRule {
        ACLRuleConfig config;
        ParsedCIDR parsedCIDR;
    };

    /**
     * @brief Check if an IP matches a parsed CIDR.
     */
    bool matchesCIDR(const std::string& ip, const ParsedCIDR& cidr) const;

    /**
     * @brief Log a rate limit violation.
     */
    void logRateLimitViolation(const std::string& clientIP, uint32_t attemptCount);

    /**
     * @brief Clean up expired attempts from sliding window.
     */
    void cleanupExpiredAttempts(RateLimitState& state);

    // Configuration
    AccessControlConfig config_;
    mutable std::shared_mutex configMutex_;

    // ACL rules storage - map by rule ID
    std::map<std::string, ParsedACLRule> aclRules_;
    mutable std::shared_mutex aclMutex_;

    // Rate limiting state - map by client IP
    std::unordered_map<std::string, RateLimitState> rateLimitState_;
    mutable std::mutex rateLimitMutex_;

    // Logging callback
    RateLimitLogCallback logCallback_;
    mutable std::mutex logCallbackMutex_;
};

} // namespace core
} // namespace openrtmp

#endif // OPENRTMP_CORE_ACCESS_CONTROL_HPP
