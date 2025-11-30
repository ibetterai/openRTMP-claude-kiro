// OpenRTMP - Cross-platform RTMP Server
// External Authentication Callback Implementation
//
// Thread-safe implementation with:
// - HTTP POST to external auth service
// - Circuit breaker pattern for fault tolerance
// - Result caching for performance
//
// Requirements coverage:
// - Requirement 15.4: Integration with external authentication services via HTTP callbacks
// - Requirement 20.4: Circuit breaker patterns for external service calls

#include "openrtmp/core/external_auth_callback.hpp"

#include <sstream>
#include <algorithm>

namespace openrtmp {
namespace core {

// =============================================================================
// Constructor / Destructor
// =============================================================================

ExternalAuthCallback::ExternalAuthCallback(
    const ExternalAuthConfig& config,
    std::shared_ptr<IHttpClient> httpClient
)
    : config_(config)
    , httpClient_(std::move(httpClient))
{
}

ExternalAuthCallback::~ExternalAuthCallback() = default;

ExternalAuthCallback::ExternalAuthCallback(ExternalAuthCallback&& other) noexcept
    : config_(std::move(other.config_))
    , httpClient_(std::move(other.httpClient_))
    , cache_(std::move(other.cache_))
    , circuitState_(other.circuitState_.load())
    , failureCount_(other.failureCount_.load())
    , lastFailureTime_(std::move(other.lastFailureTime_))
    , stats_(std::move(other.stats_))
{
}

ExternalAuthCallback& ExternalAuthCallback::operator=(ExternalAuthCallback&& other) noexcept {
    if (this != &other) {
        std::unique_lock<std::shared_mutex> configLock(configMutex_);
        std::unique_lock<std::shared_mutex> cacheLock(cacheMutex_);
        std::unique_lock<std::mutex> circuitLock(circuitMutex_);
        std::unique_lock<std::mutex> statsLock(statsMutex_);

        config_ = std::move(other.config_);
        httpClient_ = std::move(other.httpClient_);
        cache_ = std::move(other.cache_);
        circuitState_ = other.circuitState_.load();
        failureCount_ = other.failureCount_.load();
        lastFailureTime_ = std::move(other.lastFailureTime_);
        stats_ = std::move(other.stats_);
    }
    return *this;
}

// =============================================================================
// IExternalAuthCallback Implementation
// =============================================================================

AuthResult ExternalAuthCallback::authenticate(
    const std::string& app,
    const std::string& streamKey,
    const ClientInfo& client,
    AuthAction action
) {
    // Update statistics
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.totalRequests++;
    }

    // Generate cache key
    std::string cacheKey = generateCacheKey(app, streamKey, client.ip, action);

    // Check cache first (if enabled)
    bool cacheEnabled;
    {
        std::shared_lock<std::shared_mutex> lock(configMutex_);
        cacheEnabled = config_.cacheEnabled;
    }

    if (cacheEnabled) {
        auto cachedResult = checkCache(cacheKey);
        if (cachedResult) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.cacheHits++;
            stats_.successCount++;
            return *cachedResult;
        }
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.cacheMisses++;
        }
    }

    // Check circuit breaker
    if (!circuitAllowsRequest()) {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.failureCount++;
        return AuthResult::deny("Authentication service unavailable (circuit open)");
    }

    // Make the request
    AuthResult result = makeRequest(app, streamKey, client, action);

    // Record result for circuit breaker
    if (result.allowed) {
        recordSuccess();
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.successCount++;
        }

        // Cache positive results only
        if (cacheEnabled) {
            storeInCache(cacheKey, result);
        }
    } else {
        // Check if this was a service failure vs auth denial
        // Auth denials don't count as circuit breaker failures
        if (result.reason && result.reason->find("service") != std::string::npos) {
            recordFailure();
        }
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.failureCount++;
        }
    }

    return result;
}

CircuitState ExternalAuthCallback::getCircuitState() const {
    return circuitState_.load();
}

void ExternalAuthCallback::clearCache() {
    std::unique_lock<std::shared_mutex> lock(cacheMutex_);
    cache_.clear();
}

ExternalAuthStats ExternalAuthCallback::getStatistics() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void ExternalAuthCallback::updateConfig(const ExternalAuthConfig& config) {
    std::unique_lock<std::shared_mutex> lock(configMutex_);
    config_ = config;
}

ExternalAuthConfig ExternalAuthCallback::getConfig() const {
    std::shared_lock<std::shared_mutex> lock(configMutex_);
    return config_;
}

// =============================================================================
// Private Methods
// =============================================================================

std::string ExternalAuthCallback::generateCacheKey(
    const std::string& app,
    const std::string& streamKey,
    const std::string& clientIP,
    AuthAction action
) const {
    std::ostringstream oss;
    oss << app << ":"
        << streamKey << ":"
        << clientIP << ":"
        << (action == AuthAction::Publish ? "publish" : "play");
    return oss.str();
}

std::optional<AuthResult> ExternalAuthCallback::checkCache(const std::string& cacheKey) {
    std::shared_lock<std::shared_mutex> lock(cacheMutex_);

    auto it = cache_.find(cacheKey);
    if (it == cache_.end()) {
        return std::nullopt;
    }

    // Check if entry is expired
    if (std::chrono::steady_clock::now() > it->second.expireTime) {
        // Entry expired - need to upgrade to exclusive lock to remove
        lock.unlock();
        std::unique_lock<std::shared_mutex> writeLock(cacheMutex_);
        cache_.erase(cacheKey);
        return std::nullopt;
    }

    return it->second.result;
}

void ExternalAuthCallback::storeInCache(const std::string& cacheKey, const AuthResult& result) {
    uint32_t ttlMs;
    {
        std::shared_lock<std::shared_mutex> lock(configMutex_);
        ttlMs = config_.cacheTtlMs;
    }

    CacheEntry entry;
    entry.result = result;
    entry.expireTime = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(ttlMs);

    std::unique_lock<std::shared_mutex> lock(cacheMutex_);
    cache_[cacheKey] = entry;
}

std::string ExternalAuthCallback::buildRequestBody(
    const std::string& app,
    const std::string& streamKey,
    const ClientInfo& client,
    AuthAction action
) const {
    // Build JSON request body manually to avoid external JSON library dependency
    std::ostringstream json;
    json << "{";
    json << "\"app\":\"" << app << "\",";
    json << "\"streamKey\":\"" << streamKey << "\",";
    json << "\"clientIP\":\"" << client.ip << "\",";
    json << "\"action\":\"" << (action == AuthAction::Publish ? "publish" : "play") << "\"";
    json << "}";
    return json.str();
}

AuthResult ExternalAuthCallback::parseResponse(const std::string& body) {
    // Simple JSON parsing without external library dependency
    // Look for "allowed" field
    if (body.empty()) {
        return AuthResult::deny("Empty response from auth service");
    }

    // Find "allowed" field
    size_t allowedPos = body.find("\"allowed\"");
    if (allowedPos == std::string::npos) {
        return AuthResult::deny("Invalid response format: missing 'allowed' field");
    }

    // Find the value after "allowed":
    size_t colonPos = body.find(':', allowedPos);
    if (colonPos == std::string::npos) {
        return AuthResult::deny("Invalid response format");
    }

    // Check for true/false
    std::string valueArea = body.substr(colonPos + 1, 20);

    // Trim whitespace and find the value
    bool allowed = false;
    if (valueArea.find("true") != std::string::npos) {
        allowed = true;
    } else if (valueArea.find("false") != std::string::npos) {
        allowed = false;
    } else {
        return AuthResult::deny("Invalid response format: invalid 'allowed' value");
    }

    if (allowed) {
        return AuthResult::allow();
    }

    // Try to extract reason if present
    std::string reason = "Authentication denied by external service";
    size_t reasonPos = body.find("\"reason\"");
    if (reasonPos != std::string::npos) {
        size_t reasonColonPos = body.find(':', reasonPos);
        if (reasonColonPos != std::string::npos) {
            size_t reasonStartQuote = body.find('"', reasonColonPos + 1);
            if (reasonStartQuote != std::string::npos) {
                size_t reasonEndQuote = body.find('"', reasonStartQuote + 1);
                if (reasonEndQuote != std::string::npos) {
                    reason = body.substr(reasonStartQuote + 1, reasonEndQuote - reasonStartQuote - 1);
                }
            }
        }
    }

    return AuthResult::deny(reason);
}

bool ExternalAuthCallback::circuitAllowsRequest() {
    CircuitState state = circuitState_.load();

    if (state == CircuitState::Closed) {
        return true;
    }

    if (state == CircuitState::Open) {
        // Check if we should transition to half-open
        std::lock_guard<std::mutex> lock(circuitMutex_);

        uint32_t resetMs;
        {
            std::shared_lock<std::shared_mutex> configLock(configMutex_);
            resetMs = config_.circuitBreakerResetMs;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastFailureTime_
        );

        if (elapsed.count() >= resetMs) {
            // Transition to half-open
            circuitState_ = CircuitState::HalfOpen;
            return true;
        }

        return false;
    }

    // Half-open: allow one request to test
    return true;
}

void ExternalAuthCallback::recordSuccess() {
    CircuitState currentState = circuitState_.load();

    if (currentState == CircuitState::HalfOpen) {
        // Success in half-open state, close the circuit
        std::lock_guard<std::mutex> lock(circuitMutex_);
        circuitState_ = CircuitState::Closed;
        failureCount_ = 0;
    } else if (currentState == CircuitState::Closed) {
        // Reset failure count on success
        failureCount_ = 0;
    }
}

void ExternalAuthCallback::recordFailure() {
    CircuitState currentState = circuitState_.load();

    if (currentState == CircuitState::HalfOpen) {
        // Failure in half-open state, reopen the circuit
        std::lock_guard<std::mutex> lock(circuitMutex_);
        circuitState_ = CircuitState::Open;
        lastFailureTime_ = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> statsLock(statsMutex_);
            stats_.circuitOpenCount++;
        }
    } else if (currentState == CircuitState::Closed) {
        // Increment failure count
        uint32_t failures = ++failureCount_;

        uint32_t threshold;
        {
            std::shared_lock<std::shared_mutex> configLock(configMutex_);
            threshold = config_.circuitBreakerThreshold;
        }

        if (failures >= threshold) {
            // Open the circuit
            std::lock_guard<std::mutex> lock(circuitMutex_);
            circuitState_ = CircuitState::Open;
            lastFailureTime_ = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> statsLock(statsMutex_);
                stats_.circuitOpenCount++;
            }
        }
    }
}

AuthResult ExternalAuthCallback::makeRequest(
    const std::string& app,
    const std::string& streamKey,
    const ClientInfo& client,
    AuthAction action
) {
    std::string callbackUrl;
    uint32_t timeoutMs;
    AuthFallbackBehavior fallbackBehavior;

    {
        std::shared_lock<std::shared_mutex> lock(configMutex_);
        callbackUrl = config_.callbackUrl;
        timeoutMs = config_.timeoutMs;
        fallbackBehavior = config_.fallbackOnTimeout;
    }

    HttpRequest request;
    request.body = buildRequestBody(app, streamKey, client, action);
    request.contentType = "application/json";
    request.method = "POST";

    auto result = httpClient_->post(
        callbackUrl,
        request,
        std::chrono::milliseconds(timeoutMs)
    );

    if (result.isError()) {
        // Handle error based on type
        const auto& error = result.error();

        if (error.code == HttpError::Code::Timeout) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.timeoutCount++;

            // Apply fallback behavior
            if (fallbackBehavior == AuthFallbackBehavior::Allow) {
                return AuthResult::allow();
            }
            recordFailure();
            return AuthResult::deny("Authentication service timed out");
        }

        // Other errors - record failure and deny
        recordFailure();
        return AuthResult::deny("Authentication service error: " + error.message);
    }

    // Check HTTP status code
    const auto& response = result.value();
    if (response.statusCode != 200) {
        recordFailure();
        return AuthResult::deny("Authentication service returned error status: " +
                               std::to_string(response.statusCode));
    }

    // Parse response
    return parseResponse(response.body);
}

// =============================================================================
// SimpleHttpClient Implementation
// =============================================================================

Result<HttpResponse, HttpError> SimpleHttpClient::post(
    const std::string& /* url */,
    const HttpRequest& /* request */,
    std::chrono::milliseconds /* timeout */
) {
    // This is a placeholder implementation.
    // In a real implementation, this would:
    // 1. Parse the URL to extract host, port, path
    // 2. Create a TCP connection
    // 3. Send HTTP POST request with headers and body
    // 4. Read and parse HTTP response
    // 5. Handle timeout using platform-specific mechanisms
    //
    // For production use, consider using:
    // - libcurl
    // - cpp-httplib
    // - Boost.Beast
    // - Platform-native HTTP APIs (NSURLSession, WinHTTP, etc.)

    HttpError error;
    error.code = HttpError::Code::Unknown;
    error.message = "SimpleHttpClient is a stub implementation. Use a real HTTP library.";
    return Result<HttpResponse, HttpError>::error(error);
}

} // namespace core
} // namespace openrtmp
