// OpenRTMP - Cross-platform RTMP Server
// Integration Tests: TLS/RTMPS Connection
//
// Task 21.2: Implement integration test suite
// Tests TLS/RTMPS connection establishment
//
// Requirements coverage:
// - Requirement 16.1: RTMPS (RTMP over TLS) support
// - Requirement 16.2: TLS 1.2 or higher requirement

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>

#include "openrtmp/core/tls_service.hpp"
#include "openrtmp/protocol/handshake_handler.hpp"
#include "openrtmp/core/types.hpp"

namespace openrtmp {
namespace integration {
namespace test {

// =============================================================================
// Test Fixtures
// =============================================================================

class TLSConnectionIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        tlsService_ = std::make_unique<core::TLSService>();
        handshakeHandler_ = std::make_unique<protocol::HandshakeHandler>();
    }

    void TearDown() override {
        handshakeHandler_.reset();
        if (tlsService_->isInitialized()) {
            tlsService_->shutdown();
        }
        tlsService_.reset();
    }

    // Helper to simulate RTMP handshake
    bool simulateRTMPHandshake() {
        // C0: Version byte
        uint8_t c0 = 3;
        auto result = handshakeHandler_->processData(&c0, 1);
        if (!result.success) return false;

        // C1: 1536 bytes
        std::vector<uint8_t> c1(protocol::handshake::C1_SIZE);
        uint32_t timestamp = 0;
        c1[0] = (timestamp >> 24) & 0xFF;
        c1[1] = (timestamp >> 16) & 0xFF;
        c1[2] = (timestamp >> 8) & 0xFF;
        c1[3] = timestamp & 0xFF;
        c1[4] = c1[5] = c1[6] = c1[7] = 0;
        for (size_t i = 8; i < c1.size(); ++i) {
            c1[i] = static_cast<uint8_t>(i & 0xFF);
        }

        result = handshakeHandler_->processData(c1.data(), c1.size());
        if (!result.success) return false;

        auto response = handshakeHandler_->getResponseData();
        if (response.size() != protocol::handshake::S0_SIZE +
                              protocol::handshake::S1_SIZE +
                              protocol::handshake::S2_SIZE) {
            return false;
        }

        // C2: Echo S1
        std::vector<uint8_t> c2(protocol::handshake::C2_SIZE);
        std::copy(response.begin() + protocol::handshake::S0_SIZE,
                  response.begin() + protocol::handshake::S0_SIZE + protocol::handshake::S1_SIZE,
                  c2.begin());

        result = handshakeHandler_->processData(c2.data(), c2.size());
        return result.success && handshakeHandler_->isComplete();
    }

    std::unique_ptr<core::TLSService> tlsService_;
    std::unique_ptr<protocol::HandshakeHandler> handshakeHandler_;
};

// =============================================================================
// TLS Service Initialization Tests
// =============================================================================

TEST_F(TLSConnectionIntegrationTest, TLSServiceInitialization) {
    EXPECT_FALSE(tlsService_->isInitialized());

    auto result = tlsService_->initialize();
    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(tlsService_->isInitialized());
}

TEST_F(TLSConnectionIntegrationTest, TLSServiceDoubleInitialization) {
    auto result1 = tlsService_->initialize();
    EXPECT_TRUE(result1.isSuccess());

    auto result2 = tlsService_->initialize();
    EXPECT_TRUE(result2.isError());
    EXPECT_EQ(result2.error().code, core::TLSErrorCode::AlreadyInitialized);
}

TEST_F(TLSConnectionIntegrationTest, TLSServiceShutdown) {
    tlsService_->initialize();
    EXPECT_TRUE(tlsService_->isInitialized());

    tlsService_->shutdown();
    EXPECT_FALSE(tlsService_->isInitialized());
}

TEST_F(TLSConnectionIntegrationTest, TLSServiceMultipleShutdownsSafe) {
    tlsService_->initialize();
    tlsService_->shutdown();
    tlsService_->shutdown();  // Should not crash
    tlsService_->shutdown();  // Should not crash
    EXPECT_FALSE(tlsService_->isInitialized());
}

// =============================================================================
// TLS Version Configuration Tests (Requirement 16.2)
// =============================================================================

TEST_F(TLSConnectionIntegrationTest, MinimumTLSVersionIs12) {
    tlsService_->initialize();

    core::TLSConfig config;
    // Default should be TLS 1.2 minimum
    EXPECT_GE(static_cast<int>(config.minVersion), static_cast<int>(core::TLSVersion::TLS_1_2));
}

TEST_F(TLSConnectionIntegrationTest, TLS10Rejected) {
    tlsService_->initialize();

    core::TLSConfig config;
    config.minVersion = core::TLSVersion::TLS_1_0;

    auto result = tlsService_->createContext(config);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, core::TLSErrorCode::UnsupportedVersion);
}

TEST_F(TLSConnectionIntegrationTest, TLS11Rejected) {
    tlsService_->initialize();

    core::TLSConfig config;
    config.minVersion = core::TLSVersion::TLS_1_1;

    auto result = tlsService_->createContext(config);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, core::TLSErrorCode::UnsupportedVersion);
}

TEST_F(TLSConnectionIntegrationTest, TLS12Accepted) {
    tlsService_->initialize();

    core::TLSConfig config;
    config.minVersion = core::TLSVersion::TLS_1_2;

    auto result = tlsService_->createContext(config);
    EXPECT_TRUE(result.isSuccess());

    if (result.isSuccess()) {
        tlsService_->destroyContext(result.value());
    }
}

TEST_F(TLSConnectionIntegrationTest, TLS13Accepted) {
    tlsService_->initialize();

    core::TLSConfig config;
    config.minVersion = core::TLSVersion::TLS_1_3;

    auto result = tlsService_->createContext(config);
    EXPECT_TRUE(result.isSuccess());

    if (result.isSuccess()) {
        tlsService_->destroyContext(result.value());
    }
}

// =============================================================================
// TLS Context Management Tests
// =============================================================================

TEST_F(TLSConnectionIntegrationTest, CreateTLSContext) {
    tlsService_->initialize();

    core::TLSConfig config;
    config.minVersion = core::TLSVersion::TLS_1_2;

    auto result = tlsService_->createContext(config);
    ASSERT_TRUE(result.isSuccess());

    auto context = result.value();
    EXPECT_TRUE(context.isValid());

    tlsService_->destroyContext(context);
}

TEST_F(TLSConnectionIntegrationTest, CreateMultipleContexts) {
    tlsService_->initialize();

    core::TLSConfig config;
    config.minVersion = core::TLSVersion::TLS_1_2;

    std::vector<core::TLSContextHandle> contexts;
    for (int i = 0; i < 5; ++i) {
        auto result = tlsService_->createContext(config);
        ASSERT_TRUE(result.isSuccess());
        contexts.push_back(result.value());
    }

    // All contexts should be valid and unique
    for (size_t i = 0; i < contexts.size(); ++i) {
        EXPECT_TRUE(contexts[i].isValid());
        for (size_t j = i + 1; j < contexts.size(); ++j) {
            EXPECT_NE(contexts[i].id, contexts[j].id);
        }
    }

    // Clean up
    for (auto& ctx : contexts) {
        tlsService_->destroyContext(ctx);
    }
}

TEST_F(TLSConnectionIntegrationTest, CreateContextWithoutInitialization) {
    // Don't initialize TLS service

    core::TLSConfig config;
    config.minVersion = core::TLSVersion::TLS_1_2;

    auto result = tlsService_->createContext(config);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, core::TLSErrorCode::NotInitialized);
}

// =============================================================================
// Certificate Management Tests
// =============================================================================

TEST_F(TLSConnectionIntegrationTest, LoadCertificateRequiresInit) {
    auto result = tlsService_->loadCertificate("/path/to/cert.pem", "/path/to/key.pem");
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, core::TLSErrorCode::NotInitialized);
}

TEST_F(TLSConnectionIntegrationTest, LoadCertificateWithEmptyPath) {
    tlsService_->initialize();

    auto result = tlsService_->loadCertificate("", "/path/to/key.pem");
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, core::TLSErrorCode::InvalidCertificatePath);
}

TEST_F(TLSConnectionIntegrationTest, LoadCertificateWithEmptyKeyPath) {
    tlsService_->initialize();

    auto result = tlsService_->loadCertificate("/path/to/cert.pem", "");
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, core::TLSErrorCode::InvalidKeyPath);
}

TEST_F(TLSConnectionIntegrationTest, NoCertificateInitially) {
    tlsService_->initialize();
    EXPECT_FALSE(tlsService_->hasCertificate());
}

// =============================================================================
// Certificate Pinning Tests (Requirement 16.6 via Task 11.2)
// =============================================================================

TEST_F(TLSConnectionIntegrationTest, AddCertificatePin) {
    tlsService_->initialize();

    auto result = tlsService_->addCertificatePin(
        "sha256",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
    );

    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(tlsService_->getCertificatePinCount(), 1u);
}

TEST_F(TLSConnectionIntegrationTest, AddMultipleCertificatePins) {
    tlsService_->initialize();

    tlsService_->addCertificatePin("sha256", "pin1AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");
    tlsService_->addCertificatePin("sha256", "pin2BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB=");
    tlsService_->addCertificatePin("sha256", "pin3CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC=");

    EXPECT_EQ(tlsService_->getCertificatePinCount(), 3u);
}

TEST_F(TLSConnectionIntegrationTest, RemoveCertificatePin) {
    tlsService_->initialize();

    const std::string pinHash = "testpinAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    tlsService_->addCertificatePin("sha256", pinHash);
    EXPECT_EQ(tlsService_->getCertificatePinCount(), 1u);

    auto result = tlsService_->removeCertificatePin(pinHash);
    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(tlsService_->getCertificatePinCount(), 0u);
}

TEST_F(TLSConnectionIntegrationTest, ClearAllCertificatePins) {
    tlsService_->initialize();

    tlsService_->addCertificatePin("sha256", "pin1AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");
    tlsService_->addCertificatePin("sha256", "pin2BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB=");

    tlsService_->clearCertificatePins();
    EXPECT_EQ(tlsService_->getCertificatePinCount(), 0u);
}

TEST_F(TLSConnectionIntegrationTest, PinEnforcementModeDefault) {
    tlsService_->initialize();

    // Default should be enforce mode
    EXPECT_EQ(tlsService_->getPinEnforcementMode(), core::PinEnforcementMode::Enforce);
}

TEST_F(TLSConnectionIntegrationTest, SetPinEnforcementMode) {
    tlsService_->initialize();

    tlsService_->setPinEnforcementMode(core::PinEnforcementMode::ReportOnly);
    EXPECT_EQ(tlsService_->getPinEnforcementMode(), core::PinEnforcementMode::ReportOnly);

    tlsService_->setPinEnforcementMode(core::PinEnforcementMode::Enforce);
    EXPECT_EQ(tlsService_->getPinEnforcementMode(), core::PinEnforcementMode::Enforce);
}

// =============================================================================
// Certificate Expiration Monitoring Tests
// =============================================================================

TEST_F(TLSConnectionIntegrationTest, ExpirationWarningDaysDefault) {
    tlsService_->initialize();

    EXPECT_EQ(tlsService_->getExpirationWarningDays(), 30u);
}

TEST_F(TLSConnectionIntegrationTest, SetExpirationWarningDays) {
    tlsService_->initialize();

    tlsService_->setExpirationWarningDays(60);
    EXPECT_EQ(tlsService_->getExpirationWarningDays(), 60u);
}

TEST_F(TLSConnectionIntegrationTest, ExpirationCriticalDaysDefault) {
    tlsService_->initialize();

    EXPECT_EQ(tlsService_->getExpirationCriticalDays(), 7u);
}

TEST_F(TLSConnectionIntegrationTest, SetExpirationCriticalDays) {
    tlsService_->initialize();

    tlsService_->setExpirationCriticalDays(14);
    EXPECT_EQ(tlsService_->getExpirationCriticalDays(), 14u);
}

TEST_F(TLSConnectionIntegrationTest, ExpirationCallbackNotSetInitially) {
    tlsService_->initialize();

    EXPECT_FALSE(tlsService_->hasExpirationCallback());
}

TEST_F(TLSConnectionIntegrationTest, SetExpirationCallback) {
    tlsService_->initialize();

    bool callbackSet = false;
    tlsService_->setExpirationCallback([&callbackSet](const core::CertificateExpirationInfo&) {
        callbackSet = true;
    });

    EXPECT_TRUE(tlsService_->hasExpirationCallback());
}

TEST_F(TLSConnectionIntegrationTest, GetExpirationInfoWithoutCertificate) {
    tlsService_->initialize();

    auto result = tlsService_->getCertificateExpirationInfo();
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, core::TLSErrorCode::NoCertificateLoaded);
}

// =============================================================================
// Mutual TLS (mTLS) Tests
// =============================================================================

TEST_F(TLSConnectionIntegrationTest, MTLSNotEnabledByDefault) {
    tlsService_->initialize();

    EXPECT_FALSE(tlsService_->isMTLSEnabled());
}

TEST_F(TLSConnectionIntegrationTest, ConfigureMTLS) {
    tlsService_->initialize();

    core::MTLSConfig config;
    config.enabled = true;
    config.requireClientCert = true;
    config.verifyDepth = 4;

    auto result = tlsService_->configureMTLS(config);
    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(tlsService_->isMTLSEnabled());
}

TEST_F(TLSConnectionIntegrationTest, ClientCertCallbackNotSetInitially) {
    tlsService_->initialize();

    EXPECT_FALSE(tlsService_->hasClientCertificateCallback());
}

TEST_F(TLSConnectionIntegrationTest, SetClientCertCallback) {
    tlsService_->initialize();

    tlsService_->setClientCertificateCallback([](const core::ClientCertificateInfo&) {
        return true;
    });

    EXPECT_TRUE(tlsService_->hasClientCertificateCallback());
}

// =============================================================================
// Chain Verification Tests
// =============================================================================

TEST_F(TLSConnectionIntegrationTest, ChainVerificationDepthDefault) {
    tlsService_->initialize();

    EXPECT_EQ(tlsService_->getChainVerificationDepth(), 10u);
}

TEST_F(TLSConnectionIntegrationTest, SetChainVerificationDepth) {
    tlsService_->initialize();

    tlsService_->setChainVerificationDepth(5);
    EXPECT_EQ(tlsService_->getChainVerificationDepth(), 5u);
}

TEST_F(TLSConnectionIntegrationTest, HostnameVerificationEnabledByDefault) {
    tlsService_->initialize();

    EXPECT_TRUE(tlsService_->isHostnameVerificationEnabled());
}

TEST_F(TLSConnectionIntegrationTest, DisableHostnameVerification) {
    tlsService_->initialize();

    tlsService_->setHostnameVerificationEnabled(false);
    EXPECT_FALSE(tlsService_->isHostnameVerificationEnabled());
}

// =============================================================================
// RTMP over TLS Integration Tests
// =============================================================================

TEST_F(TLSConnectionIntegrationTest, RTMPHandshakeWorksInIsolation) {
    // First verify RTMP handshake works without TLS
    ASSERT_TRUE(simulateRTMPHandshake());
    EXPECT_TRUE(handshakeHandler_->isComplete());
}

TEST_F(TLSConnectionIntegrationTest, TLSContextForRTMPS) {
    tlsService_->initialize();

    // Create context suitable for RTMPS
    core::TLSConfig config;
    config.minVersion = core::TLSVersion::TLS_1_2;
    config.allowSelfSigned = true;  // For development/testing

    auto result = tlsService_->createContext(config);
    ASSERT_TRUE(result.isSuccess());

    auto context = result.value();
    EXPECT_TRUE(context.isValid());

    // Context is ready for RTMPS connections
    tlsService_->destroyContext(context);
}

TEST_F(TLSConnectionIntegrationTest, DefaultCipherSuites) {
    core::TLSConfig config;
    config.minVersion = core::TLSVersion::TLS_1_2;

    auto ciphers = config.getDefaultCipherSuites();
    EXPECT_FALSE(ciphers.empty());

    // Should include secure cipher suites
    bool hasSecureCipher = false;
    for (const auto& cipher : ciphers) {
        if (cipher.find("AES") != std::string::npos ||
            cipher.find("CHACHA") != std::string::npos) {
            hasSecureCipher = true;
            break;
        }
    }
    EXPECT_TRUE(hasSecureCipher);
}

TEST_F(TLSConnectionIntegrationTest, TLS13CipherSuites) {
    core::TLSConfig config;
    config.minVersion = core::TLSVersion::TLS_1_3;

    auto ciphers = config.getDefaultCipherSuites();
    EXPECT_FALSE(ciphers.empty());

    // TLS 1.3 cipher suites should start with TLS_
    for (const auto& cipher : ciphers) {
        EXPECT_EQ(cipher.substr(0, 4), "TLS_");
    }
}

// =============================================================================
// Port Configuration Tests (Requirement 16.5)
// =============================================================================

TEST_F(TLSConnectionIntegrationTest, PortConfigDefaults) {
    core::TLSPortConfig portConfig;

    EXPECT_EQ(portConfig.rtmpPort, 1935);
    EXPECT_EQ(portConfig.rtmpsPort, 1936);
}

TEST_F(TLSConnectionIntegrationTest, PortConfigValidation) {
    core::TLSPortConfig validConfig;
    validConfig.rtmpPort = 1935;
    validConfig.rtmpsPort = 1936;
    EXPECT_TRUE(validConfig.isValid());

    // Same port for both - invalid
    core::TLSPortConfig invalidConfig1;
    invalidConfig1.rtmpPort = 1935;
    invalidConfig1.rtmpsPort = 1935;
    EXPECT_FALSE(invalidConfig1.isValid());

    // Zero port - invalid
    core::TLSPortConfig invalidConfig2;
    invalidConfig2.rtmpPort = 0;
    invalidConfig2.rtmpsPort = 1936;
    EXPECT_FALSE(invalidConfig2.isValid());
}

// =============================================================================
// Complete TLS Connection Flow Test
// =============================================================================

TEST_F(TLSConnectionIntegrationTest, CompleteTLSSetupFlow) {
    // Step 1: Initialize TLS service
    auto initResult = tlsService_->initialize();
    ASSERT_TRUE(initResult.isSuccess());

    // Step 2: Create TLS context
    core::TLSConfig config;
    config.minVersion = core::TLSVersion::TLS_1_2;
    config.allowSelfSigned = true;

    auto contextResult = tlsService_->createContext(config);
    ASSERT_TRUE(contextResult.isSuccess());
    auto context = contextResult.value();

    // Step 3: Configure certificate pinning (optional)
    tlsService_->addCertificatePin(
        "sha256",
        "examplepinAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
    );

    // Step 4: Set expiration callback (optional)
    tlsService_->setExpirationCallback([](const core::CertificateExpirationInfo& info) {
        // Handle expiration warning
    });

    // Step 5: Context is ready for RTMPS connections
    EXPECT_TRUE(context.isValid());
    EXPECT_EQ(tlsService_->getCertificatePinCount(), 1u);
    EXPECT_TRUE(tlsService_->hasExpirationCallback());

    // Clean up
    tlsService_->destroyContext(context);
    tlsService_->shutdown();
}

} // namespace test
} // namespace integration
} // namespace openrtmp
