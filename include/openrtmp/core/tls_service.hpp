// OpenRTMP - Cross-platform RTMP Server
// TLS Service - Provides TLS/SSL encryption for RTMPS connections
//
// Responsibilities:
// - Load TLS certificates and private keys from file paths
// - Require minimum TLS 1.2 version with configurable cipher suites
// - Wrap TCP connections with TLS encryption layer
// - Support self-signed certificates for development environments
// - Operate RTMP and RTMPS on separate configurable ports
//
// Requirements coverage:
// - Requirement 16.1: RTMPS (RTMP over TLS) support
// - Requirement 16.2: TLS 1.2 or higher requirement
// - Requirement 16.3: Configurable TLS certificates including self-signed
// - Requirement 16.5: Separate ports for RTMP and RTMPS

#ifndef OPENRTMP_CORE_TLS_SERVICE_HPP
#define OPENRTMP_CORE_TLS_SERVICE_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <mutex>
#include <functional>
#include <chrono>

#include "openrtmp/core/result.hpp"

namespace openrtmp {
namespace core {

// =============================================================================
// TLS Version Enumeration
// =============================================================================

/**
 * @brief Supported TLS protocol versions.
 *
 * Per requirement 16.2, minimum supported version is TLS 1.2.
 * TLS 1.0 and 1.1 are included for configuration validation (to reject them).
 */
enum class TLSVersion : uint8_t {
    TLS_1_0 = 0,    ///< TLS 1.0 - NOT SUPPORTED (rejected)
    TLS_1_1 = 1,    ///< TLS 1.1 - NOT SUPPORTED (rejected)
    TLS_1_2 = 2,    ///< TLS 1.2 - Minimum supported version
    TLS_1_3 = 3     ///< TLS 1.3 - Highest supported version
};

// =============================================================================
// TLS Error Codes
// =============================================================================

/**
 * @brief Error codes specific to TLS operations.
 */
enum class TLSErrorCode : uint32_t {
    Success = 0,
    Unknown = 1,

    // Configuration errors (100-199)
    UnsupportedVersion = 100,       ///< TLS version below minimum
    InvalidCipherSuite = 101,       ///< Invalid or unsupported cipher suite
    ConfigurationFailed = 102,      ///< General configuration failure

    // Certificate errors (200-299)
    CertificateLoadFailed = 200,    ///< Failed to load certificate file
    KeyLoadFailed = 201,            ///< Failed to load private key file
    CertificateInvalid = 202,       ///< Certificate format is invalid
    KeyInvalid = 203,               ///< Private key format is invalid
    CertKeyMismatch = 204,          ///< Certificate and key do not match
    CertificateExpired = 205,       ///< Certificate has expired
    CertificateNotYetValid = 206,   ///< Certificate is not yet valid
    InvalidCertificatePath = 207,   ///< Certificate path is empty or invalid
    InvalidKeyPath = 208,           ///< Key path is empty or invalid

    // Context errors (300-399)
    ContextCreationFailed = 300,    ///< Failed to create TLS context
    InvalidContext = 301,           ///< Invalid or null context handle

    // Connection errors (400-499)
    InvalidSocket = 400,            ///< Invalid socket handle
    WrapFailed = 401,               ///< Failed to wrap connection with TLS
    HandshakeFailed = 402,          ///< TLS handshake failed
    ConnectionClosed = 403,         ///< Connection was closed during handshake

    // Initialization errors (500-599)
    NotInitialized = 500,           ///< TLS service not initialized
    AlreadyInitialized = 501,       ///< TLS service already initialized
    SSLLibraryInitFailed = 502,     ///< SSL library initialization failed

    // ACME errors (600-699)
    ACMERequestFailed = 600,        ///< ACME certificate request failed
    ACMEChallengeFailed = 601,      ///< ACME challenge validation failed
    ACMENotConfigured = 602,        ///< ACME not configured
    ACMEInvalidConfig = 603,        ///< ACME configuration invalid

    // Certificate pinning errors (700-799)
    InvalidPinHash = 700,           ///< Invalid pin hash value
    InvalidHashAlgorithm = 701,     ///< Invalid hash algorithm
    PinValidationFailed = 702,      ///< Certificate pin validation failed

    // mTLS errors (800-899)
    CACertificateLoadFailed = 800,  ///< Failed to load CA certificate
    CADirectoryLoadFailed = 801,    ///< Failed to load CA directory
    ClientCertRequired = 802,       ///< Client certificate required but not provided
    ClientCertInvalid = 803,        ///< Client certificate is invalid

    // Expiration/renewal errors (900-999)
    NoCertificateLoaded = 900,      ///< No certificate currently loaded
    RenewalFailed = 901,            ///< Certificate renewal failed
};

/**
 * @brief Convert TLS error code to human-readable string.
 * @param code The TLS error code
 * @return String representation of the error code
 */
inline const char* tlsErrorCodeToString(TLSErrorCode code) {
    switch (code) {
        case TLSErrorCode::Success: return "Success";
        case TLSErrorCode::Unknown: return "Unknown TLS error";
        case TLSErrorCode::UnsupportedVersion: return "Unsupported TLS version";
        case TLSErrorCode::InvalidCipherSuite: return "Invalid cipher suite";
        case TLSErrorCode::ConfigurationFailed: return "TLS configuration failed";
        case TLSErrorCode::CertificateLoadFailed: return "Certificate load failed";
        case TLSErrorCode::KeyLoadFailed: return "Key load failed";
        case TLSErrorCode::CertificateInvalid: return "Certificate invalid";
        case TLSErrorCode::KeyInvalid: return "Key invalid";
        case TLSErrorCode::CertKeyMismatch: return "Certificate and key mismatch";
        case TLSErrorCode::CertificateExpired: return "Certificate expired";
        case TLSErrorCode::CertificateNotYetValid: return "Certificate not yet valid";
        case TLSErrorCode::InvalidCertificatePath: return "Invalid certificate path";
        case TLSErrorCode::InvalidKeyPath: return "Invalid key path";
        case TLSErrorCode::ContextCreationFailed: return "Context creation failed";
        case TLSErrorCode::InvalidContext: return "Invalid TLS context";
        case TLSErrorCode::InvalidSocket: return "Invalid socket";
        case TLSErrorCode::WrapFailed: return "Wrap failed";
        case TLSErrorCode::HandshakeFailed: return "TLS handshake failed";
        case TLSErrorCode::ConnectionClosed: return "Connection closed";
        case TLSErrorCode::NotInitialized: return "TLS not initialized";
        case TLSErrorCode::AlreadyInitialized: return "TLS already initialized";
        case TLSErrorCode::SSLLibraryInitFailed: return "SSL library init failed";
        // ACME errors
        case TLSErrorCode::ACMERequestFailed: return "ACME request failed";
        case TLSErrorCode::ACMEChallengeFailed: return "ACME challenge failed";
        case TLSErrorCode::ACMENotConfigured: return "ACME not configured";
        case TLSErrorCode::ACMEInvalidConfig: return "ACME invalid configuration";
        // Certificate pinning errors
        case TLSErrorCode::InvalidPinHash: return "Invalid pin hash";
        case TLSErrorCode::InvalidHashAlgorithm: return "Invalid hash algorithm";
        case TLSErrorCode::PinValidationFailed: return "Pin validation failed";
        // mTLS errors
        case TLSErrorCode::CACertificateLoadFailed: return "CA certificate load failed";
        case TLSErrorCode::CADirectoryLoadFailed: return "CA directory load failed";
        case TLSErrorCode::ClientCertRequired: return "Client certificate required";
        case TLSErrorCode::ClientCertInvalid: return "Client certificate invalid";
        // Expiration/renewal errors
        case TLSErrorCode::NoCertificateLoaded: return "No certificate loaded";
        case TLSErrorCode::RenewalFailed: return "Certificate renewal failed";
        default: return "Unknown TLS error code";
    }
}

// =============================================================================
// TLS Error Structure
// =============================================================================

/**
 * @brief Error structure for TLS operations.
 */
struct TLSError {
    TLSErrorCode code;          ///< Error code
    std::string message;        ///< Human-readable error message

    /**
     * @brief Construct a TLS error.
     * @param c Error code
     * @param msg Error message
     */
    TLSError(TLSErrorCode c = TLSErrorCode::Unknown, std::string msg = "")
        : code(c)
        , message(std::move(msg))
    {
        if (message.empty()) {
            message = tlsErrorCodeToString(code);
        }
    }

    /**
     * @brief Get formatted error string.
     * @return Formatted error string
     */
    [[nodiscard]] std::string toString() const {
        return std::string(tlsErrorCodeToString(code)) + ": " + message;
    }
};

// =============================================================================
// TLS Configuration
// =============================================================================

/**
 * @brief Configuration for TLS/SSL connections.
 *
 * Configures TLS parameters including minimum version, cipher suites,
 * certificate pinning, and client verification.
 */
struct TLSConfig {
    TLSVersion minVersion = TLSVersion::TLS_1_2;    ///< Minimum TLS version (default: TLS 1.2)
    std::vector<std::string> cipherSuites;          ///< Allowed cipher suites (empty = defaults)
    std::optional<std::string> pinnedCertHash;      ///< Optional certificate hash for pinning
    bool verifyClient = false;                      ///< Whether to verify client certificates
    bool allowSelfSigned = true;                    ///< Allow self-signed certificates (dev mode)

    /**
     * @brief Get default cipher suites for the configured TLS version.
     * @return Vector of default cipher suite strings
     */
    [[nodiscard]] std::vector<std::string> getDefaultCipherSuites() const {
        if (minVersion >= TLSVersion::TLS_1_3) {
            return {
                "TLS_AES_256_GCM_SHA384",
                "TLS_CHACHA20_POLY1305_SHA256",
                "TLS_AES_128_GCM_SHA256"
            };
        } else {
            return {
                "ECDHE-RSA-AES256-GCM-SHA384",
                "ECDHE-RSA-AES128-GCM-SHA256",
                "ECDHE-RSA-AES256-SHA384",
                "ECDHE-RSA-AES128-SHA256"
            };
        }
    }
};

// =============================================================================
// TLS Port Configuration
// =============================================================================

/**
 * @brief Port configuration for RTMP and RTMPS services.
 *
 * Per requirement 16.5, RTMP and RTMPS operate on separate ports.
 */
struct TLSPortConfig {
    uint16_t rtmpPort = 1935;       ///< Standard RTMP port
    uint16_t rtmpsPort = 1936;      ///< RTMPS (TLS) port

    /**
     * @brief Validate port configuration.
     * @return true if configuration is valid
     */
    [[nodiscard]] bool isValid() const {
        return rtmpPort > 0 && rtmpsPort > 0 && rtmpPort != rtmpsPort;
    }
};

// =============================================================================
// TLS Handle Types
// =============================================================================

/**
 * @brief Opaque handle for TLS context.
 *
 * Wraps the platform-specific SSL_CTX or equivalent.
 */
struct TLSContextHandle {
    uint64_t id = 0;    ///< Internal context identifier

    [[nodiscard]] bool isValid() const { return id != 0; }
};

/**
 * @brief Opaque handle for TLS connection.
 *
 * Wraps the platform-specific SSL connection or equivalent.
 */
struct TLSConnectionHandle {
    uint64_t id = 0;    ///< Internal connection identifier

    [[nodiscard]] bool isValid() const { return id != 0; }
};

/**
 * @brief Type alias for platform socket handle.
 */
using SocketHandle = int;

// =============================================================================
// ACME Protocol Types - Task 11.2 (Requirement 16.4)
// =============================================================================

/**
 * @brief ACME key types for certificate generation.
 */
enum class ACMEKeyType : uint8_t {
    RSA2048 = 0,    ///< RSA 2048-bit key
    RSA4096 = 1,    ///< RSA 4096-bit key
    EC_P256 = 2,    ///< ECDSA P-256 curve
    EC_P384 = 3     ///< ECDSA P-384 curve
};

/**
 * @brief ACME challenge types for domain validation.
 */
enum class ACMEChallengeType : uint8_t {
    HTTP01 = 0,     ///< HTTP-01 challenge (port 80)
    DNS01 = 1,      ///< DNS-01 challenge (DNS TXT record)
    TLSALPN01 = 2   ///< TLS-ALPN-01 challenge (port 443)
};

/**
 * @brief ACME configuration for automatic certificate management.
 */
struct ACMEConfig {
    std::string directoryUrl = "https://acme-v02.api.letsencrypt.org/directory";
    std::string email;                              ///< Account email for Let's Encrypt
    std::vector<std::string> domains;               ///< Domains to request certificate for
    ACMEChallengeType challengeType = ACMEChallengeType::HTTP01;
    ACMEKeyType keyType = ACMEKeyType::RSA2048;     ///< Key type for certificate
    bool useStaging = false;                        ///< Use staging environment for testing
    bool acceptTermsOfService = false;              ///< Accept CA terms of service
};

// =============================================================================
// Certificate Pinning Types - Task 11.2 (Requirement 16.6)
// =============================================================================

/**
 * @brief Pin enforcement mode.
 */
enum class PinEnforcementMode : uint8_t {
    Enforce = 0,        ///< Reject connections on pin mismatch
    ReportOnly = 1      ///< Log but don't reject on mismatch
};

/**
 * @brief Individual certificate pin.
 */
struct CertificatePin {
    std::string algorithm;      ///< Hash algorithm (e.g., "sha256")
    std::string hash;           ///< Base64-encoded hash of certificate
    bool isPrimary = false;     ///< Is this the primary pin
};

/**
 * @brief Certificate pinning configuration.
 */
struct CertPinningConfig {
    std::vector<CertificatePin> pins;   ///< List of certificate pins
    PinEnforcementMode mode = PinEnforcementMode::Enforce;
    bool includeSubdomains = false;     ///< Apply pins to subdomains
};

/**
 * @brief Raw certificate data for validation.
 */
struct CertificateData {
    std::vector<uint8_t> derEncoded;    ///< DER-encoded certificate
    std::string pemEncoded;              ///< PEM-encoded certificate (optional)
};

// =============================================================================
// Certificate Expiration Types - Task 11.2
// =============================================================================

/**
 * @brief Certificate expiration status.
 */
enum class CertificateExpirationStatus : uint8_t {
    Valid = 0,          ///< Certificate is valid and not expiring soon
    ExpiringSoon = 1,   ///< Certificate is expiring within warning threshold
    Critical = 2,       ///< Certificate is expiring within critical threshold
    Expired = 3,        ///< Certificate has expired
    NotYetValid = 4     ///< Certificate is not yet valid
};

/**
 * @brief Certificate expiration information.
 */
struct CertificateExpirationInfo {
    CertificateExpirationStatus status;
    std::chrono::system_clock::time_point notBefore;
    std::chrono::system_clock::time_point notAfter;
    uint32_t daysUntilExpiration;
    std::string subject;
    std::string issuer;
};

/**
 * @brief Expiration callback function type.
 */
using ExpirationCallback = std::function<void(const CertificateExpirationInfo&)>;

/**
 * @brief Renewal configuration for automatic certificate renewal.
 */
struct RenewalConfig {
    bool autoRenew = false;             ///< Enable automatic renewal
    uint32_t renewalDaysBefore = 30;    ///< Days before expiration to renew
    uint32_t maxRetryAttempts = 3;      ///< Maximum retry attempts
    uint32_t retryIntervalHours = 6;    ///< Hours between retry attempts
};

// =============================================================================
// Mutual TLS (mTLS) Types - Task 11.2
// =============================================================================

/**
 * @brief Client certificate information.
 */
struct ClientCertificateInfo {
    std::string subject;                ///< Certificate subject DN
    std::string issuer;                 ///< Certificate issuer DN
    std::string serialNumber;           ///< Certificate serial number
    std::chrono::system_clock::time_point notBefore;
    std::chrono::system_clock::time_point notAfter;
    std::string fingerprintSHA256;      ///< SHA-256 fingerprint
    std::vector<std::string> sanDns;    ///< Subject Alternative Names (DNS)
    std::vector<std::string> sanIp;     ///< Subject Alternative Names (IP)
};

/**
 * @brief Client certificate validation callback.
 */
using ClientCertCallback = std::function<bool(const ClientCertificateInfo&)>;

/**
 * @brief Mutual TLS configuration.
 */
struct MTLSConfig {
    bool enabled = false;               ///< Enable mTLS
    bool requireClientCert = false;     ///< Require client certificate
    uint32_t verifyDepth = 4;           ///< Maximum certificate chain depth
    bool checkCRL = false;              ///< Check Certificate Revocation Lists
    std::string crlPath;                ///< Path to CRL file
    bool enableOCSPStapling = false;    ///< Enable OCSP stapling
    std::string ocspResponderUrl;       ///< OCSP responder URL
};

// =============================================================================
// Certificate Chain Verification Types - Task 11.2
// =============================================================================

/**
 * @brief Chain verification error codes.
 */
enum class ChainVerificationErrorCode : uint8_t {
    None = 0,
    CertificateExpired = 1,
    CertificateNotYetValid = 2,
    SelfSigned = 3,
    UnknownCA = 4,
    ChainTooLong = 5,
    SignatureInvalid = 6,
    HostnameMismatch = 7,
    Revoked = 8
};

/**
 * @brief Result of certificate chain verification.
 */
struct ChainVerificationResult {
    bool valid = false;
    ChainVerificationErrorCode errorCode = ChainVerificationErrorCode::None;
    std::string errorMessage;
    uint32_t chainDepth = 0;
    std::vector<std::string> chainSubjects;     ///< Subjects in the chain
};

/**
 * @brief Chain verification configuration.
 */
struct ChainVerificationConfig {
    uint32_t maxDepth = 10;             ///< Maximum chain depth
    bool verifyHostname = true;         ///< Verify hostname matches certificate
    bool allowPartialChain = false;     ///< Allow partial chain verification
    std::vector<std::string> trustedCAs;///< Paths to trusted CA certificates
};

// =============================================================================
// TLS Service Interface
// =============================================================================

/**
 * @brief Interface for TLS service operations.
 *
 * Defines the contract for TLS/SSL encryption services.
 * This interface abstracts the underlying TLS implementation
 * (OpenSSL, BoringSSL, or platform-native TLS).
 */
class ITLSService {
public:
    virtual ~ITLSService() = default;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Initialize the TLS service.
     *
     * Must be called before any other TLS operations.
     *
     * @return Success or error result
     */
    virtual Result<void, TLSError> initialize() = 0;

    /**
     * @brief Shutdown the TLS service.
     *
     * Releases all TLS resources. Safe to call multiple times.
     */
    virtual void shutdown() = 0;

    // -------------------------------------------------------------------------
    // Context Management
    // -------------------------------------------------------------------------

    /**
     * @brief Create a TLS context with the specified configuration.
     *
     * @param config TLS configuration parameters
     * @return TLS context handle or error
     */
    virtual Result<TLSContextHandle, TLSError> createContext(
        const TLSConfig& config
    ) = 0;

    /**
     * @brief Destroy a TLS context.
     *
     * @param context Context handle to destroy
     */
    virtual void destroyContext(TLSContextHandle context) = 0;

    // -------------------------------------------------------------------------
    // Connection Wrapping
    // -------------------------------------------------------------------------

    /**
     * @brief Wrap a raw socket with TLS encryption.
     *
     * @param socket Raw socket handle to wrap
     * @param context TLS context to use for the connection
     * @return TLS connection handle or error
     */
    virtual Result<TLSConnectionHandle, TLSError> wrapConnection(
        SocketHandle socket,
        TLSContextHandle context
    ) = 0;

    /**
     * @brief Unwrap and close a TLS connection.
     *
     * @param connection TLS connection to close
     */
    virtual void closeConnection(TLSConnectionHandle connection) = 0;

    // -------------------------------------------------------------------------
    // Certificate Management
    // -------------------------------------------------------------------------

    /**
     * @brief Load certificate and private key from PEM files.
     *
     * @param certPath Path to certificate PEM file
     * @param keyPath Path to private key PEM file
     * @return Success or error result
     */
    virtual Result<void, TLSError> loadCertificate(
        const std::string& certPath,
        const std::string& keyPath
    ) = 0;

    /**
     * @brief Check if a certificate is currently loaded.
     *
     * @return true if certificate is loaded
     */
    virtual bool hasCertificate() const = 0;

    // -------------------------------------------------------------------------
    // Status
    // -------------------------------------------------------------------------

    /**
     * @brief Check if TLS service is initialized.
     *
     * @return true if initialized
     */
    virtual bool isInitialized() const = 0;

    // -------------------------------------------------------------------------
    // ACME Protocol - Task 11.2 (Requirement 16.4)
    // -------------------------------------------------------------------------

    /**
     * @brief Request a certificate via ACME protocol.
     *
     * @param config ACME configuration including domains and challenge type
     * @return Success or error result
     */
    virtual Result<void, TLSError> requestACMECertificate(
        const ACMEConfig& config
    ) = 0;

    // -------------------------------------------------------------------------
    // Certificate Pinning - Task 11.2 (Requirement 16.6)
    // -------------------------------------------------------------------------

    /**
     * @brief Add a certificate pin.
     *
     * @param algorithm Hash algorithm (e.g., "sha256")
     * @param pinHash Base64-encoded hash of the certificate
     * @return Success or error result
     */
    virtual Result<void, TLSError> addCertificatePin(
        const std::string& algorithm,
        const std::string& pinHash
    ) = 0;

    /**
     * @brief Remove a certificate pin.
     *
     * @param pinHash The pin hash to remove
     * @return Success or error result
     */
    virtual Result<void, TLSError> removeCertificatePin(
        const std::string& pinHash
    ) = 0;

    /**
     * @brief Clear all certificate pins.
     */
    virtual void clearCertificatePins() = 0;

    /**
     * @brief Get the number of configured certificate pins.
     *
     * @return Number of pins
     */
    virtual size_t getCertificatePinCount() const = 0;

    /**
     * @brief Validate certificate against configured pins.
     *
     * @param certData Certificate data to validate
     * @return Success or error result
     */
    virtual Result<void, TLSError> validateCertificatePin(
        const CertificateData& certData
    ) = 0;

    /**
     * @brief Set pin enforcement mode.
     *
     * @param mode Enforcement mode
     */
    virtual void setPinEnforcementMode(PinEnforcementMode mode) = 0;

    /**
     * @brief Get current pin enforcement mode.
     *
     * @return Current enforcement mode
     */
    virtual PinEnforcementMode getPinEnforcementMode() const = 0;

    // -------------------------------------------------------------------------
    // Certificate Expiration Monitoring - Task 11.2
    // -------------------------------------------------------------------------

    /**
     * @brief Get certificate expiration information.
     *
     * @return Expiration info or error if no certificate loaded
     */
    virtual Result<CertificateExpirationInfo, TLSError> getCertificateExpirationInfo() const = 0;

    /**
     * @brief Set days before expiration to trigger warning.
     *
     * @param days Warning threshold in days
     */
    virtual void setExpirationWarningDays(uint32_t days) = 0;

    /**
     * @brief Get warning threshold in days.
     *
     * @return Warning threshold
     */
    virtual uint32_t getExpirationWarningDays() const = 0;

    /**
     * @brief Set days before expiration to trigger critical alert.
     *
     * @param days Critical threshold in days
     */
    virtual void setExpirationCriticalDays(uint32_t days) = 0;

    /**
     * @brief Get critical threshold in days.
     *
     * @return Critical threshold
     */
    virtual uint32_t getExpirationCriticalDays() const = 0;

    /**
     * @brief Set expiration callback.
     *
     * @param callback Function called when expiration status changes
     */
    virtual void setExpirationCallback(ExpirationCallback callback) = 0;

    /**
     * @brief Check if expiration callback is registered.
     *
     * @return true if callback is set
     */
    virtual bool hasExpirationCallback() const = 0;

    /**
     * @brief Configure automatic certificate renewal.
     *
     * @param config Renewal configuration
     * @return Success or error result
     */
    virtual Result<void, TLSError> configureAutoRenewal(
        const RenewalConfig& config
    ) = 0;

    /**
     * @brief Manually trigger certificate renewal.
     *
     * @return Success or error result
     */
    virtual Result<void, TLSError> triggerRenewal() = 0;

    // -------------------------------------------------------------------------
    // Mutual TLS (mTLS) - Task 11.2
    // -------------------------------------------------------------------------

    /**
     * @brief Load CA certificates for client verification.
     *
     * @param caPath Path to CA certificate file
     * @return Success or error result
     */
    virtual Result<void, TLSError> loadClientCACertificates(
        const std::string& caPath
    ) = 0;

    /**
     * @brief Load CA certificates from a directory.
     *
     * @param caDir Path to directory containing CA certificates
     * @return Success or error result
     */
    virtual Result<void, TLSError> loadClientCADirectory(
        const std::string& caDir
    ) = 0;

    /**
     * @brief Configure mutual TLS settings.
     *
     * @param config mTLS configuration
     * @return Success or error result
     */
    virtual Result<void, TLSError> configureMTLS(const MTLSConfig& config) = 0;

    /**
     * @brief Check if mTLS is enabled.
     *
     * @return true if mTLS is enabled
     */
    virtual bool isMTLSEnabled() const = 0;

    /**
     * @brief Set client certificate validation callback.
     *
     * @param callback Function called to validate client certificates
     */
    virtual void setClientCertificateCallback(ClientCertCallback callback) = 0;

    /**
     * @brief Check if client certificate callback is registered.
     *
     * @return true if callback is set
     */
    virtual bool hasClientCertificateCallback() const = 0;

    // -------------------------------------------------------------------------
    // Certificate Chain Verification - Task 11.2
    // -------------------------------------------------------------------------

    /**
     * @brief Set maximum chain verification depth.
     *
     * @param depth Maximum depth
     */
    virtual void setChainVerificationDepth(uint32_t depth) = 0;

    /**
     * @brief Get maximum chain verification depth.
     *
     * @return Current depth limit
     */
    virtual uint32_t getChainVerificationDepth() const = 0;

    /**
     * @brief Add a trusted CA certificate.
     *
     * @param caPath Path to CA certificate
     * @return Success or error result
     */
    virtual Result<void, TLSError> addTrustedCA(const std::string& caPath) = 0;

    /**
     * @brief Enable or disable hostname verification.
     *
     * @param enabled Whether to verify hostname
     */
    virtual void setHostnameVerificationEnabled(bool enabled) = 0;

    /**
     * @brief Check if hostname verification is enabled.
     *
     * @return true if hostname verification is enabled
     */
    virtual bool isHostnameVerificationEnabled() const = 0;
};

// =============================================================================
// TLS Service Implementation
// =============================================================================

/**
 * @brief Thread-safe TLS service implementation.
 *
 * Implements the ITLSService interface using OpenSSL/BoringSSL
 * or platform-native TLS libraries.
 *
 * Requirements:
 * - Requirement 16.1: RTMPS support via TLS encryption
 * - Requirement 16.2: Minimum TLS 1.2, rejecting older versions
 * - Requirement 16.3: Self-signed certificate support for development
 * - Requirement 16.5: Separate port configuration for RTMP/RTMPS
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Uses std::mutex for internal state protection
 */
class TLSService : public ITLSService {
public:
    /**
     * @brief Construct a TLS service.
     */
    TLSService();

    /**
     * @brief Destructor - ensures proper cleanup.
     */
    ~TLSService() override;

    // Non-copyable
    TLSService(const TLSService&) = delete;
    TLSService& operator=(const TLSService&) = delete;

    // Movable
    TLSService(TLSService&&) noexcept;
    TLSService& operator=(TLSService&&) noexcept;

    // -------------------------------------------------------------------------
    // ITLSService Interface Implementation
    // -------------------------------------------------------------------------

    Result<void, TLSError> initialize() override;
    void shutdown() override;

    Result<TLSContextHandle, TLSError> createContext(
        const TLSConfig& config
    ) override;

    void destroyContext(TLSContextHandle context) override;

    Result<TLSConnectionHandle, TLSError> wrapConnection(
        SocketHandle socket,
        TLSContextHandle context
    ) override;

    void closeConnection(TLSConnectionHandle connection) override;

    Result<void, TLSError> loadCertificate(
        const std::string& certPath,
        const std::string& keyPath
    ) override;

    bool hasCertificate() const override;

    bool isInitialized() const override;

    // -------------------------------------------------------------------------
    // ACME Protocol Implementation - Task 11.2
    // -------------------------------------------------------------------------

    Result<void, TLSError> requestACMECertificate(
        const ACMEConfig& config
    ) override;

    // -------------------------------------------------------------------------
    // Certificate Pinning Implementation - Task 11.2
    // -------------------------------------------------------------------------

    Result<void, TLSError> addCertificatePin(
        const std::string& algorithm,
        const std::string& pinHash
    ) override;

    Result<void, TLSError> removeCertificatePin(
        const std::string& pinHash
    ) override;

    void clearCertificatePins() override;

    size_t getCertificatePinCount() const override;

    Result<void, TLSError> validateCertificatePin(
        const CertificateData& certData
    ) override;

    void setPinEnforcementMode(PinEnforcementMode mode) override;

    PinEnforcementMode getPinEnforcementMode() const override;

    // -------------------------------------------------------------------------
    // Certificate Expiration Implementation - Task 11.2
    // -------------------------------------------------------------------------

    Result<CertificateExpirationInfo, TLSError> getCertificateExpirationInfo() const override;

    void setExpirationWarningDays(uint32_t days) override;

    uint32_t getExpirationWarningDays() const override;

    void setExpirationCriticalDays(uint32_t days) override;

    uint32_t getExpirationCriticalDays() const override;

    void setExpirationCallback(ExpirationCallback callback) override;

    bool hasExpirationCallback() const override;

    Result<void, TLSError> configureAutoRenewal(
        const RenewalConfig& config
    ) override;

    Result<void, TLSError> triggerRenewal() override;

    // -------------------------------------------------------------------------
    // Mutual TLS Implementation - Task 11.2
    // -------------------------------------------------------------------------

    Result<void, TLSError> loadClientCACertificates(
        const std::string& caPath
    ) override;

    Result<void, TLSError> loadClientCADirectory(
        const std::string& caDir
    ) override;

    Result<void, TLSError> configureMTLS(const MTLSConfig& config) override;

    bool isMTLSEnabled() const override;

    void setClientCertificateCallback(ClientCertCallback callback) override;

    bool hasClientCertificateCallback() const override;

    // -------------------------------------------------------------------------
    // Certificate Chain Verification Implementation - Task 11.2
    // -------------------------------------------------------------------------

    void setChainVerificationDepth(uint32_t depth) override;

    uint32_t getChainVerificationDepth() const override;

    Result<void, TLSError> addTrustedCA(const std::string& caPath) override;

    void setHostnameVerificationEnabled(bool enabled) override;

    bool isHostnameVerificationEnabled() const override;

private:
    /**
     * @brief Internal implementation data (PIMPL pattern).
     */
    struct Impl;
    std::unique_ptr<Impl> impl_;

    mutable std::mutex mutex_;
    bool initialized_ = false;
    bool hasCertificate_ = false;

    std::string certPath_;
    std::string keyPath_;

    uint64_t nextContextId_ = 1;
    uint64_t nextConnectionId_ = 1;

    // Task 11.2 - Advanced TLS Features state
    // Certificate pinning
    std::vector<CertificatePin> certificatePins_;
    PinEnforcementMode pinEnforcementMode_ = PinEnforcementMode::Enforce;

    // Certificate expiration monitoring
    uint32_t expirationWarningDays_ = 30;
    uint32_t expirationCriticalDays_ = 7;
    ExpirationCallback expirationCallback_;

    // Renewal configuration
    RenewalConfig renewalConfig_;
    ACMEConfig acmeConfig_;
    bool acmeConfigured_ = false;

    // mTLS configuration
    MTLSConfig mtlsConfig_;
    ClientCertCallback clientCertCallback_;

    // Chain verification
    uint32_t chainVerificationDepth_ = 10;
    bool hostnameVerificationEnabled_ = true;
};

} // namespace core
} // namespace openrtmp

#endif // OPENRTMP_CORE_TLS_SERVICE_HPP
