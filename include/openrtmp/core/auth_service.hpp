// OpenRTMP - Cross-platform RTMP Server
// Authentication Service - Validates credentials and enforces access control
//
// Responsibilities:
// - Validates stream keys against configured list or external callback
// - Supports dynamic stream key generation and revocation
// - Integrates with command processor for publish/play validation
// - Logs authentication attempts with client IP and outcome
// - Returns appropriate error responses for authentication failures
//
// Requirements coverage:
// - Requirement 15.1: Stream key-based authentication for publish
// - Requirement 15.2: Validate stream keys against configured list
// - Requirement 15.3: Optional authentication for play/subscribe
// - Requirement 15.5: Reject with error and log on auth failure

#ifndef OPENRTMP_CORE_AUTH_SERVICE_HPP
#define OPENRTMP_CORE_AUTH_SERVICE_HPP

#include <cstdint>
#include <string>
#include <optional>
#include <memory>
#include <functional>
#include <set>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <random>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/result.hpp"

namespace openrtmp {
namespace core {

// =============================================================================
// Authentication Configuration
// =============================================================================

/**
 * @brief Configuration for authentication service.
 */
struct AuthConfig {
    bool publishAuthEnabled = true;     ///< Enable authentication for publish operations
    bool subscribeAuthEnabled = false;  ///< Enable authentication for subscribe operations
    size_t maxStreamKeys = 10000;       ///< Maximum number of stream keys to store
    size_t generatedKeyLength = 32;     ///< Length of auto-generated stream keys
};

// =============================================================================
// Authentication Result
// =============================================================================

/**
 * @brief Result of an authentication operation.
 */
struct AuthResult {
    bool allowed;                       ///< Whether the operation is allowed
    std::optional<std::string> reason;  ///< Reason for denial (if not allowed)

    /**
     * @brief Create an allowed result.
     */
    static AuthResult allow() {
        return {true, std::nullopt};
    }

    /**
     * @brief Create a denied result with reason.
     */
    static AuthResult deny(std::string reasonMsg) {
        return {false, std::move(reasonMsg)};
    }
};

// =============================================================================
// Authentication Action
// =============================================================================

/**
 * @brief Type of authentication action being performed.
 */
enum class AuthAction {
    Publish,    ///< Publishing/streaming action
    Subscribe   ///< Subscribing/playing action
};

// =============================================================================
// Authentication Log Entry
// =============================================================================

/**
 * @brief Log entry for authentication attempt.
 */
struct AuthLogEntry {
    std::string clientIP;                   ///< Client IP address
    uint16_t clientPort = 0;                ///< Client port
    std::string app;                        ///< Application name
    std::string streamKey;                  ///< Stream key attempted
    AuthAction action;                      ///< Type of action
    bool success;                           ///< Whether authentication succeeded
    std::optional<std::string> reason;      ///< Reason for failure (if failed)
    std::chrono::steady_clock::time_point timestamp;  ///< Timestamp of attempt

    AuthLogEntry()
        : action(AuthAction::Publish)
        , success(false)
        , timestamp(std::chrono::steady_clock::now())
    {}
};

/**
 * @brief Callback type for authentication log events.
 */
using AuthLogCallback = std::function<void(const AuthLogEntry&)>;

// =============================================================================
// ACL Types
// =============================================================================

/**
 * @brief ACL action type.
 */
enum class ACLAction {
    Allow,  ///< Allow the connection
    Deny    ///< Deny the connection
};

/**
 * @brief Access Control List rule.
 */
struct ACLRule {
    std::string id;                     ///< Unique rule identifier
    ACLAction action;                   ///< Allow or deny
    std::string ipPattern;              ///< IP pattern (CIDR or wildcard)
    std::optional<std::string> app;     ///< Optional app scope

    ACLRule() : action(ACLAction::Allow) {}
    ACLRule(std::string ruleId, ACLAction act, std::string pattern)
        : id(std::move(ruleId))
        , action(act)
        , ipPattern(std::move(pattern))
    {}
};

// =============================================================================
// Authentication Service Interface
// =============================================================================

/**
 * @brief Interface for authentication service operations.
 *
 * Defines the contract for validating stream keys and managing access control.
 */
class IAuthService {
public:
    virtual ~IAuthService() = default;

    // -------------------------------------------------------------------------
    // Stream Key Validation
    // -------------------------------------------------------------------------

    /**
     * @brief Validate a publish request.
     *
     * @param app Application name
     * @param streamKey Stream key to validate
     * @param client Client information
     * @return AuthResult indicating if publishing is allowed
     */
    virtual AuthResult validatePublish(
        const std::string& app,
        const std::string& streamKey,
        const ClientInfo& client
    ) = 0;

    /**
     * @brief Validate a subscribe/play request.
     *
     * @param app Application name
     * @param streamKey Stream key to validate
     * @param client Client information
     * @return AuthResult indicating if subscribing is allowed
     */
    virtual AuthResult validateSubscribe(
        const std::string& app,
        const std::string& streamKey,
        const ClientInfo& client
    ) = 0;

    // -------------------------------------------------------------------------
    // Stream Key Management
    // -------------------------------------------------------------------------

    /**
     * @brief Add a stream key to the allow list.
     *
     * @param streamKey Stream key to add
     * @param app Optional app scope (empty for global key)
     */
    virtual void addStreamKey(
        const std::string& streamKey,
        const std::string& app = ""
    ) = 0;

    /**
     * @brief Remove a stream key from the allow list.
     *
     * @param streamKey Stream key to remove
     * @param app Optional app scope
     */
    virtual void removeStreamKey(
        const std::string& streamKey,
        const std::string& app = ""
    ) = 0;

    /**
     * @brief Check if a stream key exists.
     *
     * @param streamKey Stream key to check
     * @return true if stream key exists
     */
    virtual bool hasStreamKey(const std::string& streamKey) const = 0;

    /**
     * @brief Generate a new unique stream key.
     *
     * The generated key is automatically added to the allow list.
     *
     * @return Generated stream key
     */
    virtual std::string generateStreamKey() = 0;

    /**
     * @brief Get the number of configured stream keys.
     *
     * @return Number of stream keys
     */
    virtual size_t getStreamKeyCount() const = 0;

    /**
     * @brief Clear all stream keys.
     */
    virtual void clearStreamKeys() = 0;

    // -------------------------------------------------------------------------
    // ACL Management
    // -------------------------------------------------------------------------

    /**
     * @brief Add an ACL rule.
     *
     * @param rule ACL rule to add
     */
    virtual void addACLRule(const ACLRule& rule) = 0;

    /**
     * @brief Remove an ACL rule by ID.
     *
     * @param ruleId Rule ID to remove
     */
    virtual void removeACLRule(const std::string& ruleId) = 0;

    // -------------------------------------------------------------------------
    // Rate Limiting
    // -------------------------------------------------------------------------

    /**
     * @brief Check if a client IP is within rate limits.
     *
     * @param clientIP Client IP to check
     * @return true if within limits, false if rate limited
     */
    virtual bool checkRateLimit(const std::string& clientIP) = 0;

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    /**
     * @brief Get current configuration.
     *
     * @return Current auth configuration
     */
    virtual AuthConfig getConfig() const = 0;

    /**
     * @brief Update configuration.
     *
     * @param config New configuration
     */
    virtual void updateConfig(const AuthConfig& config) = 0;

    // -------------------------------------------------------------------------
    // Logging
    // -------------------------------------------------------------------------

    /**
     * @brief Set the authentication log callback.
     *
     * @param callback Callback to invoke on auth attempts
     */
    virtual void setAuthLogCallback(AuthLogCallback callback) = 0;
};

// =============================================================================
// Authentication Service Implementation
// =============================================================================

/**
 * @brief Thread-safe authentication service implementation.
 *
 * Implements the IAuthService interface with:
 * - Stream key validation against configured allow list
 * - Dynamic stream key generation and revocation
 * - Application-scoped stream keys
 * - Authentication attempt logging
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses std::shared_mutex for read/write locking
 */
class AuthService : public IAuthService {
public:
    /**
     * @brief Construct an AuthService with configuration.
     *
     * @param config Authentication configuration
     */
    explicit AuthService(const AuthConfig& config = AuthConfig());

    /**
     * @brief Destructor.
     */
    ~AuthService() override;

    // Non-copyable
    AuthService(const AuthService&) = delete;
    AuthService& operator=(const AuthService&) = delete;

    // Movable
    AuthService(AuthService&&) noexcept;
    AuthService& operator=(AuthService&&) noexcept;

    // IAuthService interface implementation
    AuthResult validatePublish(
        const std::string& app,
        const std::string& streamKey,
        const ClientInfo& client
    ) override;

    AuthResult validateSubscribe(
        const std::string& app,
        const std::string& streamKey,
        const ClientInfo& client
    ) override;

    void addStreamKey(
        const std::string& streamKey,
        const std::string& app = ""
    ) override;

    void removeStreamKey(
        const std::string& streamKey,
        const std::string& app = ""
    ) override;

    bool hasStreamKey(const std::string& streamKey) const override;

    std::string generateStreamKey() override;

    size_t getStreamKeyCount() const override;

    void clearStreamKeys() override;

    void addACLRule(const ACLRule& rule) override;

    void removeACLRule(const std::string& ruleId) override;

    bool checkRateLimit(const std::string& clientIP) override;

    AuthConfig getConfig() const override;

    void updateConfig(const AuthConfig& config) override;

    void setAuthLogCallback(AuthLogCallback callback) override;

private:
    /**
     * @brief Internal key structure for app-scoped keys.
     */
    struct StreamKeyEntry {
        std::string key;
        std::string app;  // Empty for global keys

        bool operator==(const StreamKeyEntry& other) const {
            return key == other.key && app == other.app;
        }
    };

    /**
     * @brief Hash function for StreamKeyEntry.
     */
    struct StreamKeyEntryHash {
        size_t operator()(const StreamKeyEntry& entry) const {
            size_t h1 = std::hash<std::string>{}(entry.key);
            size_t h2 = std::hash<std::string>{}(entry.app);
            return h1 ^ (h2 << 1);
        }
    };

    /**
     * @brief Validate a stream key against the allow list.
     *
     * @param app Application name
     * @param streamKey Stream key to validate
     * @return true if key is valid
     */
    bool isValidStreamKey(const std::string& app, const std::string& streamKey) const;

    /**
     * @brief Log an authentication attempt.
     *
     * @param app Application name
     * @param streamKey Stream key
     * @param client Client info
     * @param action Action type
     * @param success Whether auth succeeded
     * @param reason Failure reason (if failed)
     */
    void logAuthAttempt(
        const std::string& app,
        const std::string& streamKey,
        const ClientInfo& client,
        AuthAction action,
        bool success,
        const std::optional<std::string>& reason = std::nullopt
    );

    /**
     * @brief Generate random string for stream key.
     *
     * @param length Length of string to generate
     * @return Random alphanumeric string
     */
    std::string generateRandomString(size_t length);

    // Configuration
    AuthConfig config_;
    mutable std::shared_mutex configMutex_;

    // Global stream keys (no app scope)
    std::unordered_set<std::string> globalStreamKeys_;

    // App-scoped stream keys: app -> set of keys
    std::unordered_map<std::string, std::unordered_set<std::string>> appScopedKeys_;

    // Mutex for stream keys
    mutable std::shared_mutex keysMutex_;

    // ACL rules
    std::map<std::string, ACLRule> aclRules_;
    mutable std::shared_mutex aclMutex_;

    // Rate limiting state
    std::unordered_map<std::string, std::vector<std::chrono::steady_clock::time_point>> rateLimitState_;
    mutable std::mutex rateLimitMutex_;

    // Logging callback
    AuthLogCallback logCallback_;
    mutable std::mutex logCallbackMutex_;

    // Random number generator for key generation
    std::mt19937_64 rng_;
    std::mutex rngMutex_;
};

} // namespace core
} // namespace openrtmp

#endif // OPENRTMP_CORE_AUTH_SERVICE_HPP
