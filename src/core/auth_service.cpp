// OpenRTMP - Cross-platform RTMP Server
// Authentication Service Implementation
//
// Thread-safe implementation of authentication service with:
// - Stream key validation against configured allow list
// - Dynamic stream key generation and revocation
// - Application-scoped stream keys
// - Authentication attempt logging
//
// Requirements coverage:
// - Requirement 15.1: Stream key-based authentication for publish
// - Requirement 15.2: Validate stream keys against configured list
// - Requirement 15.3: Optional authentication for play/subscribe
// - Requirement 15.5: Reject with error and log on auth failure

#include "openrtmp/core/auth_service.hpp"

#include <algorithm>
#include <sstream>
#include <iomanip>

namespace openrtmp {
namespace core {

// =============================================================================
// Constructor / Destructor
// =============================================================================

AuthService::AuthService(const AuthConfig& config)
    : config_(config)
{
    // Initialize random number generator with random device
    std::random_device rd;
    rng_.seed(rd());
}

AuthService::~AuthService() = default;

AuthService::AuthService(AuthService&& other) noexcept
    : config_(std::move(other.config_))
    , globalStreamKeys_(std::move(other.globalStreamKeys_))
    , appScopedKeys_(std::move(other.appScopedKeys_))
    , aclRules_(std::move(other.aclRules_))
    , rateLimitState_(std::move(other.rateLimitState_))
{
    std::lock_guard<std::mutex> lock(other.logCallbackMutex_);
    logCallback_ = std::move(other.logCallback_);

    std::lock_guard<std::mutex> rngLock(other.rngMutex_);
    rng_ = std::move(other.rng_);
}

AuthService& AuthService::operator=(AuthService&& other) noexcept {
    if (this != &other) {
        // Lock all mutexes
        std::unique_lock<std::shared_mutex> configLock1(configMutex_, std::defer_lock);
        std::unique_lock<std::shared_mutex> keysLock1(keysMutex_, std::defer_lock);
        std::unique_lock<std::shared_mutex> aclLock1(aclMutex_, std::defer_lock);
        std::unique_lock<std::shared_mutex> configLock2(other.configMutex_, std::defer_lock);
        std::unique_lock<std::shared_mutex> keysLock2(other.keysMutex_, std::defer_lock);
        std::unique_lock<std::shared_mutex> aclLock2(other.aclMutex_, std::defer_lock);

        std::lock(configLock1, keysLock1, aclLock1, configLock2, keysLock2, aclLock2);

        config_ = std::move(other.config_);
        globalStreamKeys_ = std::move(other.globalStreamKeys_);
        appScopedKeys_ = std::move(other.appScopedKeys_);
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

        {
            std::lock_guard<std::mutex> rngLock1(rngMutex_);
            std::lock_guard<std::mutex> rngLock2(other.rngMutex_);
            rng_ = std::move(other.rng_);
        }
    }
    return *this;
}

// =============================================================================
// Stream Key Validation
// =============================================================================

AuthResult AuthService::validatePublish(
    const std::string& app,
    const std::string& streamKey,
    const ClientInfo& client
) {
    AuthConfig currentConfig;
    {
        std::shared_lock<std::shared_mutex> lock(configMutex_);
        currentConfig = config_;
    }

    // If publish auth is disabled, allow all
    if (!currentConfig.publishAuthEnabled) {
        AuthResult result = AuthResult::allow();
        logAuthAttempt(app, streamKey, client, AuthAction::Publish, true);
        return result;
    }

    // Empty stream key is always invalid
    if (streamKey.empty()) {
        AuthResult result = AuthResult::deny("Stream key cannot be empty");
        logAuthAttempt(app, streamKey, client, AuthAction::Publish, false, result.reason);
        return result;
    }

    // Validate stream key against allow list
    if (!isValidStreamKey(app, streamKey)) {
        AuthResult result = AuthResult::deny("Invalid stream key");
        logAuthAttempt(app, streamKey, client, AuthAction::Publish, false, result.reason);
        return result;
    }

    AuthResult result = AuthResult::allow();
    logAuthAttempt(app, streamKey, client, AuthAction::Publish, true);
    return result;
}

AuthResult AuthService::validateSubscribe(
    const std::string& app,
    const std::string& streamKey,
    const ClientInfo& client
) {
    AuthConfig currentConfig;
    {
        std::shared_lock<std::shared_mutex> lock(configMutex_);
        currentConfig = config_;
    }

    // If subscribe auth is disabled, allow all
    if (!currentConfig.subscribeAuthEnabled) {
        AuthResult result = AuthResult::allow();
        logAuthAttempt(app, streamKey, client, AuthAction::Subscribe, true);
        return result;
    }

    // Empty stream key is always invalid
    if (streamKey.empty()) {
        AuthResult result = AuthResult::deny("Stream key cannot be empty");
        logAuthAttempt(app, streamKey, client, AuthAction::Subscribe, false, result.reason);
        return result;
    }

    // Validate stream key against allow list
    if (!isValidStreamKey(app, streamKey)) {
        AuthResult result = AuthResult::deny("Invalid stream key");
        logAuthAttempt(app, streamKey, client, AuthAction::Subscribe, false, result.reason);
        return result;
    }

    AuthResult result = AuthResult::allow();
    logAuthAttempt(app, streamKey, client, AuthAction::Subscribe, true);
    return result;
}

// =============================================================================
// Stream Key Management
// =============================================================================

void AuthService::addStreamKey(
    const std::string& streamKey,
    const std::string& app
) {
    std::unique_lock<std::shared_mutex> lock(keysMutex_);

    if (app.empty()) {
        // Global key
        globalStreamKeys_.insert(streamKey);
    } else {
        // App-scoped key
        appScopedKeys_[app].insert(streamKey);
    }
}

void AuthService::removeStreamKey(
    const std::string& streamKey,
    const std::string& app
) {
    std::unique_lock<std::shared_mutex> lock(keysMutex_);

    if (app.empty()) {
        // Remove from global keys
        globalStreamKeys_.erase(streamKey);

        // Also remove from all app-scoped keys
        for (auto& pair : appScopedKeys_) {
            pair.second.erase(streamKey);
        }
    } else {
        // Remove from specific app
        auto it = appScopedKeys_.find(app);
        if (it != appScopedKeys_.end()) {
            it->second.erase(streamKey);
        }
    }
}

bool AuthService::hasStreamKey(const std::string& streamKey) const {
    std::shared_lock<std::shared_mutex> lock(keysMutex_);

    // Check global keys
    if (globalStreamKeys_.count(streamKey) > 0) {
        return true;
    }

    // Check app-scoped keys
    for (const auto& pair : appScopedKeys_) {
        if (pair.second.count(streamKey) > 0) {
            return true;
        }
    }

    return false;
}

std::string AuthService::generateStreamKey() {
    size_t keyLength;
    {
        std::shared_lock<std::shared_mutex> lock(configMutex_);
        keyLength = config_.generatedKeyLength;
    }

    std::string key = generateRandomString(keyLength);

    // Add to global keys
    addStreamKey(key);

    return key;
}

size_t AuthService::getStreamKeyCount() const {
    std::shared_lock<std::shared_mutex> lock(keysMutex_);

    size_t count = globalStreamKeys_.size();
    for (const auto& pair : appScopedKeys_) {
        count += pair.second.size();
    }

    return count;
}

void AuthService::clearStreamKeys() {
    std::unique_lock<std::shared_mutex> lock(keysMutex_);

    globalStreamKeys_.clear();
    appScopedKeys_.clear();
}

// =============================================================================
// ACL Management
// =============================================================================

void AuthService::addACLRule(const ACLRule& rule) {
    std::unique_lock<std::shared_mutex> lock(aclMutex_);
    aclRules_[rule.id] = rule;
}

void AuthService::removeACLRule(const std::string& ruleId) {
    std::unique_lock<std::shared_mutex> lock(aclMutex_);
    aclRules_.erase(ruleId);
}

// =============================================================================
// Rate Limiting
// =============================================================================

bool AuthService::checkRateLimit(const std::string& clientIP) {
    std::lock_guard<std::mutex> lock(rateLimitMutex_);

    auto now = std::chrono::steady_clock::now();
    auto& attempts = rateLimitState_[clientIP];

    // Remove old attempts (older than 1 minute)
    auto oneMinuteAgo = now - std::chrono::minutes(1);
    attempts.erase(
        std::remove_if(attempts.begin(), attempts.end(),
            [oneMinuteAgo](const auto& t) { return t < oneMinuteAgo; }),
        attempts.end()
    );

    // Check if within limit (5 per minute as per requirement 15.7)
    if (attempts.size() >= 5) {
        return false;
    }

    // Record this attempt
    attempts.push_back(now);
    return true;
}

// =============================================================================
// Configuration
// =============================================================================

AuthConfig AuthService::getConfig() const {
    std::shared_lock<std::shared_mutex> lock(configMutex_);
    return config_;
}

void AuthService::updateConfig(const AuthConfig& config) {
    std::unique_lock<std::shared_mutex> lock(configMutex_);
    config_ = config;
}

// =============================================================================
// Logging
// =============================================================================

void AuthService::setAuthLogCallback(AuthLogCallback callback) {
    std::lock_guard<std::mutex> lock(logCallbackMutex_);
    logCallback_ = std::move(callback);
}

// =============================================================================
// Private Methods
// =============================================================================

bool AuthService::isValidStreamKey(const std::string& app, const std::string& streamKey) const {
    std::shared_lock<std::shared_mutex> lock(keysMutex_);

    // Check global keys first
    if (globalStreamKeys_.count(streamKey) > 0) {
        return true;
    }

    // Check app-scoped keys
    auto it = appScopedKeys_.find(app);
    if (it != appScopedKeys_.end()) {
        if (it->second.count(streamKey) > 0) {
            return true;
        }
    }

    return false;
}

void AuthService::logAuthAttempt(
    const std::string& app,
    const std::string& streamKey,
    const ClientInfo& client,
    AuthAction action,
    bool success,
    const std::optional<std::string>& reason
) {
    AuthLogCallback callback;
    {
        std::lock_guard<std::mutex> lock(logCallbackMutex_);
        callback = logCallback_;
    }

    if (callback) {
        AuthLogEntry entry;
        entry.clientIP = client.ip;
        entry.clientPort = client.port;
        entry.app = app;
        entry.streamKey = streamKey;
        entry.action = action;
        entry.success = success;
        entry.reason = reason;
        entry.timestamp = std::chrono::steady_clock::now();

        callback(entry);
    }
}

std::string AuthService::generateRandomString(size_t length) {
    static const char charset[] =
        "0123456789"
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    std::string result;
    result.reserve(length);

    std::lock_guard<std::mutex> lock(rngMutex_);
    std::uniform_int_distribution<size_t> dist(0, sizeof(charset) - 2);

    for (size_t i = 0; i < length; ++i) {
        result += charset[dist(rng_)];
    }

    return result;
}

} // namespace core
} // namespace openrtmp
