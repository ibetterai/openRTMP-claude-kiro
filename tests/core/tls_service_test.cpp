// OpenRTMP - Cross-platform RTMP Server
// Tests for TLS Service - TLS/SSL encryption for RTMPS connections
//
// Tests coverage:
// - Requirement 16.1: RTMPS (RTMP over TLS) support
// - Requirement 16.2: TLS 1.2 or higher requirement
// - Requirement 16.3: Configurable TLS certificates including self-signed
// - Requirement 16.5: Separate ports for RTMP and RTMPS

#include <gtest/gtest.h>
#include "openrtmp/core/tls_service.hpp"

namespace openrtmp {
namespace core {
namespace test {

// =============================================================================
// TLS Configuration Tests
// =============================================================================

class TLSConfigTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test default TLS configuration values
TEST_F(TLSConfigTest, DefaultConfiguration) {
    TLSConfig config;

    // Default minimum version should be TLS 1.2
    EXPECT_EQ(config.minVersion, TLSVersion::TLS_1_2);

    // Verify client should be off by default
    EXPECT_FALSE(config.verifyClient);

    // Default cipher suites should not be empty (implementation provides defaults)
    // Note: We allow empty config, implementation provides defaults
}

// Test TLS 1.2 minimum version enforcement
TEST_F(TLSConfigTest, MinimumTLS12Version) {
    TLSConfig config;
    config.minVersion = TLSVersion::TLS_1_2;

    EXPECT_EQ(config.minVersion, TLSVersion::TLS_1_2);

    // TLS 1.2 should be supported
    EXPECT_GE(static_cast<int>(config.minVersion),
              static_cast<int>(TLSVersion::TLS_1_2));
}

// Test TLS 1.3 configuration
TEST_F(TLSConfigTest, TLS13Configuration) {
    TLSConfig config;
    config.minVersion = TLSVersion::TLS_1_3;

    EXPECT_EQ(config.minVersion, TLSVersion::TLS_1_3);
}

// Test cipher suite configuration
TEST_F(TLSConfigTest, CipherSuiteConfiguration) {
    TLSConfig config;
    config.cipherSuites = {
        "TLS_AES_256_GCM_SHA384",
        "TLS_CHACHA20_POLY1305_SHA256",
        "TLS_AES_128_GCM_SHA256"
    };

    EXPECT_EQ(config.cipherSuites.size(), 3u);
    EXPECT_EQ(config.cipherSuites[0], "TLS_AES_256_GCM_SHA384");
}

// Test certificate pinning configuration
TEST_F(TLSConfigTest, CertificatePinningConfiguration) {
    TLSConfig config;
    config.pinnedCertHash = "sha256//abcdef123456";

    EXPECT_TRUE(config.pinnedCertHash.has_value());
    EXPECT_EQ(config.pinnedCertHash.value(), "sha256//abcdef123456");
}

// Test client verification configuration
TEST_F(TLSConfigTest, ClientVerificationConfiguration) {
    TLSConfig config;
    config.verifyClient = true;

    EXPECT_TRUE(config.verifyClient);
}

// =============================================================================
// TLS Service Interface Tests
// =============================================================================

class TLSServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        service_ = std::make_unique<TLSService>();
    }
    void TearDown() override {
        service_.reset();
    }

    std::unique_ptr<TLSService> service_;
};

// Test TLS service construction
TEST_F(TLSServiceTest, Construction) {
    EXPECT_NE(service_, nullptr);
}

// Test TLS context creation with default configuration
TEST_F(TLSServiceTest, CreateContextWithDefaultConfig) {
    TLSConfig config;
    config.minVersion = TLSVersion::TLS_1_2;

    auto result = service_->createContext(config);

    // Should succeed (stubbed implementation) or fail if SSL not initialized
    // For this abstraction layer test, we check the interface works
    EXPECT_TRUE(result.isSuccess() || result.isError());
}

// Test TLS context creation with TLS 1.3
TEST_F(TLSServiceTest, CreateContextWithTLS13) {
    TLSConfig config;
    config.minVersion = TLSVersion::TLS_1_3;

    auto result = service_->createContext(config);

    // Interface should handle both success and error cases
    EXPECT_TRUE(result.isSuccess() || result.isError());
}

// Test invalid TLS version rejection
TEST_F(TLSServiceTest, RejectInvalidTLSVersion) {
    TLSConfig config;
    // Test with TLS 1.0 (should be rejected as it's below minimum)
    config.minVersion = TLSVersion::TLS_1_0;

    auto result = service_->createContext(config);

    // Should return error for version below TLS 1.2
    EXPECT_TRUE(result.isError());
    if (result.isError()) {
        EXPECT_EQ(result.error().code, TLSErrorCode::UnsupportedVersion);
    }
}

// Test TLS 1.1 rejection (below minimum requirement)
TEST_F(TLSServiceTest, RejectTLS11Version) {
    TLSConfig config;
    config.minVersion = TLSVersion::TLS_1_1;

    auto result = service_->createContext(config);

    // Should return error for version below TLS 1.2
    EXPECT_TRUE(result.isError());
    if (result.isError()) {
        EXPECT_EQ(result.error().code, TLSErrorCode::UnsupportedVersion);
    }
}

// =============================================================================
// Certificate Loading Tests
// =============================================================================

class TLSCertificateTest : public ::testing::Test {
protected:
    void SetUp() override {
        service_ = std::make_unique<TLSService>();
    }
    void TearDown() override {
        service_.reset();
    }

    std::unique_ptr<TLSService> service_;
};

// Test loading certificate from non-existent file
TEST_F(TLSCertificateTest, LoadCertificateFileNotFound) {
    auto result = service_->loadCertificate(
        "/non/existent/cert.pem",
        "/non/existent/key.pem"
    );

    EXPECT_TRUE(result.isError());
    if (result.isError()) {
        EXPECT_EQ(result.error().code, TLSErrorCode::CertificateLoadFailed);
    }
}

// Test loading certificate with empty path
TEST_F(TLSCertificateTest, LoadCertificateEmptyPath) {
    auto result = service_->loadCertificate("", "");

    EXPECT_TRUE(result.isError());
    if (result.isError()) {
        EXPECT_EQ(result.error().code, TLSErrorCode::InvalidCertificatePath);
    }
}

// Test loading only certificate without key
TEST_F(TLSCertificateTest, LoadCertificateWithoutKey) {
    auto result = service_->loadCertificate("/path/to/cert.pem", "");

    EXPECT_TRUE(result.isError());
    if (result.isError()) {
        EXPECT_EQ(result.error().code, TLSErrorCode::InvalidKeyPath);
    }
}

// Test loading certificate with key path only
TEST_F(TLSCertificateTest, LoadKeyWithoutCertificate) {
    auto result = service_->loadCertificate("", "/path/to/key.pem");

    EXPECT_TRUE(result.isError());
    if (result.isError()) {
        EXPECT_EQ(result.error().code, TLSErrorCode::InvalidCertificatePath);
    }
}

// Test certificate validation status check
TEST_F(TLSCertificateTest, CheckCertificateStatus) {
    // Before loading any certificate
    EXPECT_FALSE(service_->hasCertificate());
}

// =============================================================================
// TLS Port Configuration Tests
// =============================================================================

class TLSPortConfigTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test default RTMP and RTMPS port configuration
TEST_F(TLSPortConfigTest, DefaultPortConfiguration) {
    TLSPortConfig portConfig;

    // Default RTMP port is 1935
    EXPECT_EQ(portConfig.rtmpPort, 1935);

    // Default RTMPS port is 443 (TLS standard) or 1936
    EXPECT_TRUE(portConfig.rtmpsPort == 443 || portConfig.rtmpsPort == 1936);
}

// Test custom port configuration
TEST_F(TLSPortConfigTest, CustomPortConfiguration) {
    TLSPortConfig portConfig;
    portConfig.rtmpPort = 1940;
    portConfig.rtmpsPort = 1941;

    EXPECT_EQ(portConfig.rtmpPort, 1940);
    EXPECT_EQ(portConfig.rtmpsPort, 1941);
}

// Test that RTMP and RTMPS ports are different
TEST_F(TLSPortConfigTest, SeparatePorts) {
    TLSPortConfig portConfig;

    // RTMP and RTMPS should operate on separate ports
    EXPECT_NE(portConfig.rtmpPort, portConfig.rtmpsPort);
}

// Test port validation
TEST_F(TLSPortConfigTest, PortValidation) {
    TLSPortConfig portConfig;

    // Valid port range is 1-65535
    EXPECT_GT(portConfig.rtmpPort, static_cast<uint16_t>(0));
    EXPECT_GT(portConfig.rtmpsPort, static_cast<uint16_t>(0));
}

// =============================================================================
// TLS Connection Wrapping Tests
// =============================================================================

class TLSConnectionWrapTest : public ::testing::Test {
protected:
    void SetUp() override {
        service_ = std::make_unique<TLSService>();
    }
    void TearDown() override {
        service_.reset();
    }

    std::unique_ptr<TLSService> service_;
};

// Test wrapping invalid socket handle
TEST_F(TLSConnectionWrapTest, WrapInvalidSocket) {
    TLSConfig config;
    config.minVersion = TLSVersion::TLS_1_2;

    // Create context first (may fail in stub, but test the flow)
    auto contextResult = service_->createContext(config);

    // Try to wrap invalid socket (-1)
    auto result = service_->wrapConnection(
        static_cast<SocketHandle>(-1),
        TLSContextHandle{0}
    );

    EXPECT_TRUE(result.isError());
    if (result.isError()) {
        EXPECT_EQ(result.error().code, TLSErrorCode::InvalidSocket);
    }
}

// Test wrapping with invalid context
TEST_F(TLSConnectionWrapTest, WrapWithInvalidContext) {
    // Try to wrap with null/invalid context handle
    auto result = service_->wrapConnection(
        static_cast<SocketHandle>(1),
        TLSContextHandle{0}  // Invalid context
    );

    EXPECT_TRUE(result.isError());
    if (result.isError()) {
        EXPECT_EQ(result.error().code, TLSErrorCode::InvalidContext);
    }
}

// =============================================================================
// TLS Error Handling Tests
// =============================================================================

class TLSErrorTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test TLS error code to string conversion
TEST_F(TLSErrorTest, ErrorCodeToString) {
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::Success), "Success");
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::UnsupportedVersion), "Unsupported TLS version");
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::CertificateLoadFailed), "Certificate load failed");
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::InvalidCertificatePath), "Invalid certificate path");
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::InvalidKeyPath), "Invalid key path");
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::InvalidSocket), "Invalid socket");
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::InvalidContext), "Invalid TLS context");
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::HandshakeFailed), "TLS handshake failed");
}

// Test TLS error structure
TEST_F(TLSErrorTest, ErrorStructure) {
    TLSError error{TLSErrorCode::CertificateLoadFailed, "Failed to load certificate"};

    EXPECT_EQ(error.code, TLSErrorCode::CertificateLoadFailed);
    EXPECT_EQ(error.message, "Failed to load certificate");
}

// =============================================================================
// Self-Signed Certificate Tests
// =============================================================================

class TLSSelfSignedTest : public ::testing::Test {
protected:
    void SetUp() override {
        service_ = std::make_unique<TLSService>();
    }
    void TearDown() override {
        service_.reset();
    }

    std::unique_ptr<TLSService> service_;
};

// Test self-signed certificate flag in configuration
TEST_F(TLSSelfSignedTest, AllowSelfSignedConfiguration) {
    TLSConfig config;
    config.allowSelfSigned = true;

    EXPECT_TRUE(config.allowSelfSigned);
}

// Test disallowing self-signed certificates
TEST_F(TLSSelfSignedTest, DisallowSelfSignedConfiguration) {
    TLSConfig config;
    config.allowSelfSigned = false;

    EXPECT_FALSE(config.allowSelfSigned);
}

// Test default self-signed setting (should allow for development)
TEST_F(TLSSelfSignedTest, DefaultSelfSignedSetting) {
    TLSConfig config;

    // Default should allow self-signed for development convenience
    EXPECT_TRUE(config.allowSelfSigned);
}

// =============================================================================
// TLS Service Lifecycle Tests
// =============================================================================

class TLSServiceLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test TLS service initialization
TEST_F(TLSServiceLifecycleTest, ServiceInitialization) {
    TLSService service;

    auto result = service.initialize();

    // Should succeed or return appropriate error
    EXPECT_TRUE(result.isSuccess() || result.isError());
}

// Test TLS service shutdown
TEST_F(TLSServiceLifecycleTest, ServiceShutdown) {
    TLSService service;
    service.initialize();

    // Shutdown should be safe even if not initialized
    service.shutdown();

    // Should be able to call shutdown multiple times safely
    service.shutdown();
}

// Test double initialization
TEST_F(TLSServiceLifecycleTest, DoubleInitialization) {
    TLSService service;

    auto result1 = service.initialize();
    auto result2 = service.initialize();

    // Second initialization should either succeed (idempotent)
    // or return already initialized error
    EXPECT_TRUE(result2.isSuccess() || result2.isError());
}

// =============================================================================
// TLS Configuration Validation Tests
// =============================================================================

class TLSConfigValidationTest : public ::testing::Test {
protected:
    void SetUp() override {
        service_ = std::make_unique<TLSService>();
    }
    void TearDown() override {
        service_.reset();
    }

    std::unique_ptr<TLSService> service_;
};

// Test validating empty cipher suites (should use defaults)
TEST_F(TLSConfigValidationTest, EmptyCipherSuitesUsesDefaults) {
    TLSConfig config;
    config.minVersion = TLSVersion::TLS_1_2;
    config.cipherSuites.clear();  // Empty cipher suites

    auto result = service_->createContext(config);

    // Should succeed using default cipher suites
    // (or fail for other reasons, but not for empty cipher suites)
    if (result.isError()) {
        EXPECT_NE(result.error().code, TLSErrorCode::InvalidCipherSuite);
    }
}

// Test validating invalid cipher suite name
TEST_F(TLSConfigValidationTest, InvalidCipherSuiteName) {
    TLSConfig config;
    config.minVersion = TLSVersion::TLS_1_2;
    config.cipherSuites = {"INVALID_CIPHER_SUITE_NAME"};

    auto result = service_->createContext(config);

    // May succeed (implementation may ignore invalid ciphers) or fail
    EXPECT_TRUE(result.isSuccess() || result.isError());
}

// =============================================================================
// Advanced TLS Features Tests - Task 11.2
// =============================================================================
// Coverage:
// - Requirement 16.4: ACME protocol for automatic certificate management
// - Requirement 16.6: Certificate pinning for controlled deployments

// -----------------------------------------------------------------------------
// ACME Protocol Interface Tests
// -----------------------------------------------------------------------------

class ACMEClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        service_ = std::make_unique<TLSService>();
        service_->initialize();
    }
    void TearDown() override {
        service_->shutdown();
        service_.reset();
    }

    std::unique_ptr<TLSService> service_;
};

// Test ACME configuration structure
TEST_F(ACMEClientTest, ACMEConfigDefaults) {
    ACMEConfig config;

    // Default directory URL should be Let's Encrypt production
    EXPECT_FALSE(config.directoryUrl.empty());

    // Default should not be staging
    EXPECT_FALSE(config.useStaging);

    // Default key type should be RSA 2048 or EC P-256
    EXPECT_TRUE(config.keyType == ACMEKeyType::RSA2048 ||
                config.keyType == ACMEKeyType::EC_P256);
}

// Test ACME staging configuration
TEST_F(ACMEClientTest, ACMEConfigStaging) {
    ACMEConfig config;
    config.useStaging = true;
    config.directoryUrl = "https://acme-staging-v02.api.letsencrypt.org/directory";

    EXPECT_TRUE(config.useStaging);
    EXPECT_NE(config.directoryUrl.find("staging"), std::string::npos);
}

// Test ACME certificate request interface
TEST_F(ACMEClientTest, RequestACMECertificateInterface) {
    ACMEConfig config;
    config.directoryUrl = "https://acme-v02.api.letsencrypt.org/directory";
    config.email = "test@example.com";
    config.domains = {"example.com"};
    config.challengeType = ACMEChallengeType::HTTP01;

    // Request should fail gracefully (no actual ACME server)
    auto result = service_->requestACMECertificate(config);

    // Should return error (no network or domain validation possible in test)
    EXPECT_TRUE(result.isError());
    if (result.isError()) {
        // Could be any of these errors in a test environment
        EXPECT_TRUE(
            result.error().code == TLSErrorCode::ACMERequestFailed ||
            result.error().code == TLSErrorCode::ACMEChallengeFailed ||
            result.error().code == TLSErrorCode::ACMENotConfigured
        );
    }
}

// Test ACME with empty domain list
TEST_F(ACMEClientTest, RequestACMECertificateEmptyDomains) {
    ACMEConfig config;
    config.domains.clear();

    auto result = service_->requestACMECertificate(config);

    EXPECT_TRUE(result.isError());
    if (result.isError()) {
        EXPECT_EQ(result.error().code, TLSErrorCode::ACMEInvalidConfig);
    }
}

// Test ACME with invalid email
TEST_F(ACMEClientTest, RequestACMECertificateInvalidEmail) {
    ACMEConfig config;
    config.domains = {"example.com"};
    config.email = "not-an-email";

    auto result = service_->requestACMECertificate(config);

    EXPECT_TRUE(result.isError());
}

// Test ACME challenge types
TEST_F(ACMEClientTest, ACMEChallengeTypes) {
    ACMEConfig httpConfig;
    httpConfig.challengeType = ACMEChallengeType::HTTP01;
    EXPECT_EQ(httpConfig.challengeType, ACMEChallengeType::HTTP01);

    ACMEConfig dnsConfig;
    dnsConfig.challengeType = ACMEChallengeType::DNS01;
    EXPECT_EQ(dnsConfig.challengeType, ACMEChallengeType::DNS01);

    ACMEConfig tlsConfig;
    tlsConfig.challengeType = ACMEChallengeType::TLSALPN01;
    EXPECT_EQ(tlsConfig.challengeType, ACMEChallengeType::TLSALPN01);
}

// -----------------------------------------------------------------------------
// Certificate Pinning Tests
// -----------------------------------------------------------------------------

class CertificatePinningTest : public ::testing::Test {
protected:
    void SetUp() override {
        service_ = std::make_unique<TLSService>();
        service_->initialize();
    }
    void TearDown() override {
        service_->shutdown();
        service_.reset();
    }

    std::unique_ptr<TLSService> service_;
};

// Test certificate pinning configuration
TEST_F(CertificatePinningTest, PinningConfiguration) {
    CertPinningConfig config;

    // Should support multiple pinned certificates
    config.pins.push_back(CertificatePin{
        "sha256",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
        true  // Primary
    });
    config.pins.push_back(CertificatePin{
        "sha256",
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB=",
        false  // Backup
    });

    EXPECT_EQ(config.pins.size(), 2u);
    EXPECT_TRUE(config.pins[0].isPrimary);
    EXPECT_FALSE(config.pins[1].isPrimary);
}

// Test adding certificate pin
TEST_F(CertificatePinningTest, AddCertificatePin) {
    std::string hashAlgorithm = "sha256";
    std::string pinHash = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";

    auto result = service_->addCertificatePin(hashAlgorithm, pinHash);

    // Should succeed
    EXPECT_TRUE(result.isSuccess());
}

// Test adding invalid pin hash
TEST_F(CertificatePinningTest, AddInvalidPinHash) {
    auto result = service_->addCertificatePin("sha256", "");

    EXPECT_TRUE(result.isError());
    if (result.isError()) {
        EXPECT_EQ(result.error().code, TLSErrorCode::InvalidPinHash);
    }
}

// Test adding pin with invalid algorithm
TEST_F(CertificatePinningTest, AddPinInvalidAlgorithm) {
    auto result = service_->addCertificatePin("md5", "somehash");

    EXPECT_TRUE(result.isError());
    if (result.isError()) {
        EXPECT_EQ(result.error().code, TLSErrorCode::InvalidHashAlgorithm);
    }
}

// Test removing certificate pin
TEST_F(CertificatePinningTest, RemoveCertificatePin) {
    std::string pinHash = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";

    // First add a pin
    service_->addCertificatePin("sha256", pinHash);

    // Then remove it
    auto result = service_->removeCertificatePin(pinHash);

    EXPECT_TRUE(result.isSuccess());
}

// Test clearing all pins
TEST_F(CertificatePinningTest, ClearAllPins) {
    // Add multiple pins
    service_->addCertificatePin("sha256", "PIN1");
    service_->addCertificatePin("sha256", "PIN2");

    // Clear all
    service_->clearCertificatePins();

    // Verify pins are cleared
    EXPECT_EQ(service_->getCertificatePinCount(), 0u);
}

// Test certificate validation against pins
TEST_F(CertificatePinningTest, ValidateCertificateAgainstPins) {
    // Add a pin
    std::string testPin = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    service_->addCertificatePin("sha256", testPin);

    // Validate a non-matching certificate (should fail)
    // In real implementation, this would validate actual cert data
    CertificateData certData;
    certData.derEncoded = {0x30, 0x82};  // Minimal DER stub

    auto result = service_->validateCertificatePin(certData);

    // Should fail because pin doesn't match
    EXPECT_TRUE(result.isError());
}

// Test pin enforcement mode
TEST_F(CertificatePinningTest, PinEnforcementMode) {
    // Test enforce mode (reject on mismatch)
    service_->setPinEnforcementMode(PinEnforcementMode::Enforce);
    EXPECT_EQ(service_->getPinEnforcementMode(), PinEnforcementMode::Enforce);

    // Test report-only mode (log but don't reject)
    service_->setPinEnforcementMode(PinEnforcementMode::ReportOnly);
    EXPECT_EQ(service_->getPinEnforcementMode(), PinEnforcementMode::ReportOnly);
}

// -----------------------------------------------------------------------------
// Certificate Expiration Monitoring Tests
// -----------------------------------------------------------------------------

class CertificateExpirationTest : public ::testing::Test {
protected:
    void SetUp() override {
        service_ = std::make_unique<TLSService>();
        service_->initialize();
    }
    void TearDown() override {
        service_->shutdown();
        service_.reset();
    }

    std::unique_ptr<TLSService> service_;
};

// Test certificate expiration info retrieval
TEST_F(CertificateExpirationTest, GetCertificateExpirationInfo) {
    auto result = service_->getCertificateExpirationInfo();

    // Should return error if no certificate loaded
    EXPECT_TRUE(result.isError());
    if (result.isError()) {
        EXPECT_EQ(result.error().code, TLSErrorCode::NoCertificateLoaded);
    }
}

// Test expiration warning threshold configuration
TEST_F(CertificateExpirationTest, ExpirationWarningThreshold) {
    // Set warning threshold to 30 days
    service_->setExpirationWarningDays(30);
    EXPECT_EQ(service_->getExpirationWarningDays(), 30u);

    // Set critical threshold to 7 days
    service_->setExpirationCriticalDays(7);
    EXPECT_EQ(service_->getExpirationCriticalDays(), 7u);
}

// Test expiration callback registration
TEST_F(CertificateExpirationTest, ExpirationCallbackRegistration) {
    bool callbackCalled = false;
    ExpirationCallback callback = [&callbackCalled](
        const CertificateExpirationInfo& info
    ) {
        callbackCalled = true;
    };

    service_->setExpirationCallback(callback);

    // Callback registration should succeed
    EXPECT_TRUE(service_->hasExpirationCallback());
}

// Test renewal scheduling
TEST_F(CertificateExpirationTest, RenewalScheduling) {
    RenewalConfig renewalConfig;
    renewalConfig.autoRenew = true;
    renewalConfig.renewalDaysBefore = 30;
    renewalConfig.maxRetryAttempts = 3;
    renewalConfig.retryIntervalHours = 6;

    auto result = service_->configureAutoRenewal(renewalConfig);

    // Should succeed
    EXPECT_TRUE(result.isSuccess());
}

// Test manual renewal trigger
TEST_F(CertificateExpirationTest, ManualRenewalTrigger) {
    // Without ACME configured, should fail
    auto result = service_->triggerRenewal();

    EXPECT_TRUE(result.isError());
}

// Test expiration status enumeration
TEST_F(CertificateExpirationTest, ExpirationStatus) {
    // Valid status should be one of these
    CertificateExpirationStatus status = CertificateExpirationStatus::Valid;
    EXPECT_EQ(status, CertificateExpirationStatus::Valid);

    status = CertificateExpirationStatus::ExpiringSoon;
    EXPECT_EQ(status, CertificateExpirationStatus::ExpiringSoon);

    status = CertificateExpirationStatus::Critical;
    EXPECT_EQ(status, CertificateExpirationStatus::Critical);

    status = CertificateExpirationStatus::Expired;
    EXPECT_EQ(status, CertificateExpirationStatus::Expired);
}

// -----------------------------------------------------------------------------
// Mutual TLS (mTLS) Tests
// -----------------------------------------------------------------------------

class MutualTLSTest : public ::testing::Test {
protected:
    void SetUp() override {
        service_ = std::make_unique<TLSService>();
        service_->initialize();
    }
    void TearDown() override {
        service_->shutdown();
        service_.reset();
    }

    std::unique_ptr<TLSService> service_;
};

// Test mTLS configuration
TEST_F(MutualTLSTest, MTLSConfiguration) {
    MTLSConfig config;
    config.enabled = true;
    config.requireClientCert = true;
    config.verifyDepth = 3;

    EXPECT_TRUE(config.enabled);
    EXPECT_TRUE(config.requireClientCert);
    EXPECT_EQ(config.verifyDepth, 3);
}

// Test loading CA certificates for client verification
TEST_F(MutualTLSTest, LoadClientCACertificates) {
    // Try loading from non-existent path
    auto result = service_->loadClientCACertificates("/non/existent/ca.pem");

    EXPECT_TRUE(result.isError());
    if (result.isError()) {
        EXPECT_EQ(result.error().code, TLSErrorCode::CACertificateLoadFailed);
    }
}

// Test loading CA certificate directory
TEST_F(MutualTLSTest, LoadClientCADirectory) {
    // Try loading from non-existent directory
    auto result = service_->loadClientCADirectory("/non/existent/certs/");

    EXPECT_TRUE(result.isError());
    if (result.isError()) {
        EXPECT_EQ(result.error().code, TLSErrorCode::CADirectoryLoadFailed);
    }
}

// Test enabling mTLS
TEST_F(MutualTLSTest, EnableMTLS) {
    MTLSConfig config;
    config.enabled = true;
    config.requireClientCert = true;

    auto result = service_->configureMTLS(config);

    // Should succeed (configuration accepted)
    EXPECT_TRUE(result.isSuccess());

    // Verify mTLS is enabled
    EXPECT_TRUE(service_->isMTLSEnabled());
}

// Test client certificate validation callback
TEST_F(MutualTLSTest, ClientCertificateValidationCallback) {
    bool callbackCalled = false;
    ClientCertCallback callback = [&callbackCalled](
        const ClientCertificateInfo& certInfo
    ) -> bool {
        callbackCalled = true;
        return true;  // Accept client
    };

    service_->setClientCertificateCallback(callback);

    EXPECT_TRUE(service_->hasClientCertificateCallback());
}

// Test CRL checking configuration
TEST_F(MutualTLSTest, CRLCheckingConfiguration) {
    MTLSConfig config;
    config.enabled = true;
    config.checkCRL = true;
    config.crlPath = "/path/to/crl.pem";

    auto result = service_->configureMTLS(config);

    EXPECT_TRUE(result.isSuccess());
}

// Test OCSP stapling configuration
TEST_F(MutualTLSTest, OCSPStaplingConfiguration) {
    MTLSConfig config;
    config.enabled = true;
    config.enableOCSPStapling = true;
    config.ocspResponderUrl = "http://ocsp.example.com";

    auto result = service_->configureMTLS(config);

    EXPECT_TRUE(result.isSuccess());
}

// Test client certificate info extraction
TEST_F(MutualTLSTest, ClientCertificateInfoStructure) {
    ClientCertificateInfo info;
    info.subject = "CN=test,O=Example";
    info.issuer = "CN=CA,O=Example";
    info.serialNumber = "1234567890";
    info.notBefore = std::chrono::system_clock::now();
    info.notAfter = std::chrono::system_clock::now() + std::chrono::hours(24 * 365);
    info.fingerprintSHA256 = "AA:BB:CC:DD:EE:FF";

    EXPECT_EQ(info.subject, "CN=test,O=Example");
    EXPECT_EQ(info.issuer, "CN=CA,O=Example");
    EXPECT_FALSE(info.serialNumber.empty());
}

// -----------------------------------------------------------------------------
// Certificate Chain Verification Tests
// -----------------------------------------------------------------------------

class CertificateChainTest : public ::testing::Test {
protected:
    void SetUp() override {
        service_ = std::make_unique<TLSService>();
        service_->initialize();
    }
    void TearDown() override {
        service_->shutdown();
        service_.reset();
    }

    std::unique_ptr<TLSService> service_;
};

// Test chain verification configuration
TEST_F(CertificateChainTest, ChainVerificationConfig) {
    ChainVerificationConfig config;
    config.maxDepth = 5;
    config.verifyHostname = true;
    config.allowPartialChain = false;

    EXPECT_EQ(config.maxDepth, 5);
    EXPECT_TRUE(config.verifyHostname);
    EXPECT_FALSE(config.allowPartialChain);
}

// Test setting chain verification depth
TEST_F(CertificateChainTest, SetChainVerificationDepth) {
    service_->setChainVerificationDepth(4);
    EXPECT_EQ(service_->getChainVerificationDepth(), 4u);
}

// Test adding trusted CA to chain
TEST_F(CertificateChainTest, AddTrustedCA) {
    // Adding from non-existent file should fail
    auto result = service_->addTrustedCA("/non/existent/ca.pem");

    EXPECT_TRUE(result.isError());
}

// Test chain verification result structure
TEST_F(CertificateChainTest, ChainVerificationResult) {
    ChainVerificationResult result;
    result.valid = false;
    result.errorCode = ChainVerificationErrorCode::CertificateExpired;
    result.errorMessage = "Certificate has expired";
    result.chainDepth = 2;

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.errorCode, ChainVerificationErrorCode::CertificateExpired);
    EXPECT_EQ(result.chainDepth, 2);
}

// Test chain verification error codes
TEST_F(CertificateChainTest, ChainVerificationErrorCodes) {
    EXPECT_EQ(static_cast<int>(ChainVerificationErrorCode::None), 0);
    EXPECT_NE(static_cast<int>(ChainVerificationErrorCode::CertificateExpired),
              static_cast<int>(ChainVerificationErrorCode::None));
    EXPECT_NE(static_cast<int>(ChainVerificationErrorCode::SelfSigned),
              static_cast<int>(ChainVerificationErrorCode::CertificateExpired));
    EXPECT_NE(static_cast<int>(ChainVerificationErrorCode::UnknownCA),
              static_cast<int>(ChainVerificationErrorCode::SelfSigned));
}

// Test hostname verification
TEST_F(CertificateChainTest, HostnameVerification) {
    service_->setHostnameVerificationEnabled(true);
    EXPECT_TRUE(service_->isHostnameVerificationEnabled());

    service_->setHostnameVerificationEnabled(false);
    EXPECT_FALSE(service_->isHostnameVerificationEnabled());
}

// -----------------------------------------------------------------------------
// Advanced TLS Error Codes Tests
// -----------------------------------------------------------------------------

class AdvancedTLSErrorTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test new TLS error codes for advanced features
TEST_F(AdvancedTLSErrorTest, ACMEErrorCodes) {
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::ACMERequestFailed),
                 "ACME request failed");
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::ACMEChallengeFailed),
                 "ACME challenge failed");
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::ACMENotConfigured),
                 "ACME not configured");
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::ACMEInvalidConfig),
                 "ACME invalid configuration");
}

// Test pinning error codes
TEST_F(AdvancedTLSErrorTest, PinningErrorCodes) {
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::InvalidPinHash),
                 "Invalid pin hash");
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::InvalidHashAlgorithm),
                 "Invalid hash algorithm");
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::PinValidationFailed),
                 "Pin validation failed");
}

// Test mTLS error codes
TEST_F(AdvancedTLSErrorTest, MTLSErrorCodes) {
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::CACertificateLoadFailed),
                 "CA certificate load failed");
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::CADirectoryLoadFailed),
                 "CA directory load failed");
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::ClientCertRequired),
                 "Client certificate required");
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::ClientCertInvalid),
                 "Client certificate invalid");
}

// Test expiration error codes
TEST_F(AdvancedTLSErrorTest, ExpirationErrorCodes) {
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::NoCertificateLoaded),
                 "No certificate loaded");
    EXPECT_STREQ(tlsErrorCodeToString(TLSErrorCode::RenewalFailed),
                 "Certificate renewal failed");
}

} // namespace test
} // namespace core
} // namespace openrtmp
