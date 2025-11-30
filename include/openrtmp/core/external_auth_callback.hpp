// OpenRTMP - Cross-platform RTMP Server
// External Authentication Callback - HTTP-based external auth service integration
//
// Responsibilities:
// - HTTP POST callback to external auth service
// - Send app name, stream key, client IP, and action in request
// - Handle callback timeout with configurable fallback behavior
// - Implement circuit breaker for failed auth service calls
// - Cache positive authentication results for performance
//
// Requirements coverage:
// - Requirement 15.4: Integration with external authentication services via HTTP callbacks
// - Requirement 20.4: Circuit breaker patterns for external service calls

#ifndef OPENRTMP_CORE_EXTERNAL_AUTH_CALLBACK_HPP
#define OPENRTMP_CORE_EXTERNAL_AUTH_CALLBACK_HPP

#include <cstdint>
#include <string>
#include <optional>
#include <memory>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <atomic>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/result.hpp"
#include "openrtmp/core/auth_service.hpp"

namespace openrtmp {
namespace core {

// =============================================================================
// HTTP Client Types
// =============================================================================

/**
 * @brief HTTP request structure for auth callbacks.
 */
struct HttpRequest {
    std::string url;
    std::string method = "POST";
    std::string body;
    std::string contentType = "application/json";
};

/**
 * @brief HTTP response structure from auth service.
 */
struct HttpResponse {
    int statusCode = 0;
    std::string body;
};

/**
 * @brief HTTP error codes.
 */
struct HttpError {
    enum class Code {
        None,
        ConnectionFailed,
        Timeout,
        InvalidResponse,
        Unknown
    };

    Code code = Code::None;
    std::string message;
};

/**
 * @brief Interface for HTTP client operations.
 *
 * Allows mocking of HTTP requests for testing.
 */
class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    /**
     * @brief Make an HTTP POST request.
     *
     * @param url Target URL
     * @param request Request data
     * @param timeout Request timeout
     * @return Response or error
     */
    virtual Result<HttpResponse, HttpError> post(
        const std::string& url,
        const HttpRequest& request,
        std::chrono::milliseconds timeout
    ) = 0;
};

// =============================================================================
// Circuit Breaker Types
// =============================================================================

/**
 * @brief Circuit breaker state.
 */
enum class CircuitState {
    Closed,     ///< Normal operation, requests go through
    Open,       ///< Service is down, requests fail fast
    HalfOpen    ///< Testing if service recovered
};

// =============================================================================
// External Auth Configuration
// =============================================================================

/**
 * @brief Fallback behavior when external auth times out.
 */
enum class AuthFallbackBehavior {
    Allow,  ///< Allow on timeout (permissive)
    Deny    ///< Deny on timeout (secure)
};

/**
 * @brief Configuration for external authentication callback.
 */
struct ExternalAuthConfig {
    std::string callbackUrl;                    ///< URL for auth callback POST requests
    uint32_t timeoutMs = 5000;                  ///< Request timeout in milliseconds (default 5s)
    AuthFallbackBehavior fallbackOnTimeout = AuthFallbackBehavior::Deny;  ///< Behavior on timeout

    // Circuit breaker configuration
    uint32_t circuitBreakerThreshold = 5;       ///< Failures before opening circuit
    uint32_t circuitBreakerResetMs = 30000;     ///< Time before trying again (30s default)

    // Cache configuration
    bool cacheEnabled = true;                   ///< Enable result caching
    uint32_t cacheTtlMs = 60000;                ///< Cache TTL in milliseconds (1 minute default)
};

// =============================================================================
// Statistics
// =============================================================================

/**
 * @brief Statistics for external auth callback.
 */
struct ExternalAuthStats {
    uint64_t totalRequests = 0;     ///< Total authentication requests
    uint64_t successCount = 0;      ///< Successful authentications
    uint64_t failureCount = 0;      ///< Failed authentications
    uint64_t timeoutCount = 0;      ///< Timeout count
    uint64_t cacheHits = 0;         ///< Cache hit count
    uint64_t cacheMisses = 0;       ///< Cache miss count
    uint64_t circuitOpenCount = 0;  ///< Times circuit was opened
};

// =============================================================================
// External Auth Callback Interface
// =============================================================================

/**
 * @brief Interface for external authentication callback.
 */
class IExternalAuthCallback {
public:
    virtual ~IExternalAuthCallback() = default;

    /**
     * @brief Authenticate against external service.
     *
     * @param app Application name
     * @param streamKey Stream key
     * @param client Client information
     * @param action Authentication action (publish/subscribe)
     * @return Authentication result
     */
    virtual AuthResult authenticate(
        const std::string& app,
        const std::string& streamKey,
        const ClientInfo& client,
        AuthAction action
    ) = 0;

    /**
     * @brief Get current circuit breaker state.
     * @return Current circuit state
     */
    virtual CircuitState getCircuitState() const = 0;

    /**
     * @brief Clear the authentication cache.
     */
    virtual void clearCache() = 0;

    /**
     * @brief Get statistics.
     * @return Current statistics
     */
    virtual ExternalAuthStats getStatistics() const = 0;

    /**
     * @brief Update configuration.
     * @param config New configuration
     */
    virtual void updateConfig(const ExternalAuthConfig& config) = 0;

    /**
     * @brief Get current configuration.
     * @return Current configuration
     */
    virtual ExternalAuthConfig getConfig() const = 0;
};

// =============================================================================
// External Auth Callback Implementation
// =============================================================================

/**
 * @brief Implementation of external authentication callback.
 *
 * Thread-safe implementation with:
 * - HTTP POST to external auth service
 * - Circuit breaker pattern for fault tolerance
 * - Result caching for performance
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses std::shared_mutex for config and cache
 * - Uses std::mutex for circuit breaker state
 */
class ExternalAuthCallback : public IExternalAuthCallback {
public:
    /**
     * @brief Construct with configuration and HTTP client.
     *
     * @param config External auth configuration
     * @param httpClient HTTP client for making requests
     */
    ExternalAuthCallback(
        const ExternalAuthConfig& config,
        std::shared_ptr<IHttpClient> httpClient
    );

    /**
     * @brief Destructor.
     */
    ~ExternalAuthCallback() override;

    // Non-copyable
    ExternalAuthCallback(const ExternalAuthCallback&) = delete;
    ExternalAuthCallback& operator=(const ExternalAuthCallback&) = delete;

    // Movable
    ExternalAuthCallback(ExternalAuthCallback&&) noexcept;
    ExternalAuthCallback& operator=(ExternalAuthCallback&&) noexcept;

    // IExternalAuthCallback implementation
    AuthResult authenticate(
        const std::string& app,
        const std::string& streamKey,
        const ClientInfo& client,
        AuthAction action
    ) override;

    CircuitState getCircuitState() const override;

    void clearCache() override;

    ExternalAuthStats getStatistics() const override;

    void updateConfig(const ExternalAuthConfig& config) override;

    ExternalAuthConfig getConfig() const override;

private:
    // =============================================================================
    // Cache Entry
    // =============================================================================
    struct CacheEntry {
        AuthResult result;
        std::chrono::steady_clock::time_point expireTime;
    };

    // =============================================================================
    // Private Methods
    // =============================================================================

    /**
     * @brief Generate cache key from auth parameters.
     */
    std::string generateCacheKey(
        const std::string& app,
        const std::string& streamKey,
        const std::string& clientIP,
        AuthAction action
    ) const;

    /**
     * @brief Check cache for existing result.
     */
    std::optional<AuthResult> checkCache(const std::string& cacheKey);

    /**
     * @brief Store result in cache.
     */
    void storeInCache(const std::string& cacheKey, const AuthResult& result);

    /**
     * @brief Build JSON request body.
     */
    std::string buildRequestBody(
        const std::string& app,
        const std::string& streamKey,
        const ClientInfo& client,
        AuthAction action
    ) const;

    /**
     * @brief Parse JSON response body.
     */
    AuthResult parseResponse(const std::string& body);

    /**
     * @brief Check if circuit breaker allows request.
     */
    bool circuitAllowsRequest();

    /**
     * @brief Record circuit breaker success.
     */
    void recordSuccess();

    /**
     * @brief Record circuit breaker failure.
     */
    void recordFailure();

    /**
     * @brief Make the actual HTTP request.
     */
    AuthResult makeRequest(
        const std::string& app,
        const std::string& streamKey,
        const ClientInfo& client,
        AuthAction action
    );

    // =============================================================================
    // Member Variables
    // =============================================================================

    // Configuration
    ExternalAuthConfig config_;
    mutable std::shared_mutex configMutex_;

    // HTTP client
    std::shared_ptr<IHttpClient> httpClient_;

    // Cache
    std::unordered_map<std::string, CacheEntry> cache_;
    mutable std::shared_mutex cacheMutex_;

    // Circuit breaker state
    std::atomic<CircuitState> circuitState_{CircuitState::Closed};
    std::atomic<uint32_t> failureCount_{0};
    std::chrono::steady_clock::time_point lastFailureTime_;
    mutable std::mutex circuitMutex_;

    // Statistics
    mutable std::mutex statsMutex_;
    ExternalAuthStats stats_;
};

// =============================================================================
// Simple HTTP Client Implementation
// =============================================================================

/**
 * @brief Simple HTTP client implementation using platform-native facilities.
 *
 * This is a basic implementation that can be replaced with more sophisticated
 * HTTP libraries (libcurl, cpp-httplib, etc.) for production use.
 */
class SimpleHttpClient : public IHttpClient {
public:
    SimpleHttpClient() = default;
    ~SimpleHttpClient() override = default;

    Result<HttpResponse, HttpError> post(
        const std::string& url,
        const HttpRequest& request,
        std::chrono::milliseconds timeout
    ) override;
};

} // namespace core
} // namespace openrtmp

#endif // OPENRTMP_CORE_EXTERNAL_AUTH_CALLBACK_HPP
