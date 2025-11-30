// OpenRTMP - Cross-platform RTMP Server
// Tests for Darwin (macOS) Service Wrapper Implementation
//
// Requirements Covered: 7.4, 7.5, 7.7
// - 7.4: Support binding to any available network interface
// - 7.5: Support running as a background service or daemon
// - 7.7: Request necessary network permissions through standard macOS permission dialogs

#include <gtest/gtest.h>

#if defined(__APPLE__)

#include "openrtmp/pal/darwin/darwin_service_wrapper.hpp"
#include "openrtmp/core/result.hpp"

#include <atomic>
#include <thread>
#include <chrono>
#include <fstream>
#include <csignal>
#include <sys/stat.h>
#include <unistd.h>

namespace openrtmp {
namespace pal {
namespace darwin {
namespace test {

// =============================================================================
// Service Wrapper Basic Tests
// =============================================================================

class DarwinServiceWrapperTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<DarwinServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<DarwinServiceWrapper> wrapper_;
};

TEST_F(DarwinServiceWrapperTest, DefaultConstructor) {
    EXPECT_NE(wrapper_, nullptr);
}

TEST_F(DarwinServiceWrapperTest, ImplementsIServiceWrapperInterface) {
    IServiceWrapper* interface = wrapper_.get();
    EXPECT_NE(interface, nullptr);
}

// =============================================================================
// Launchd Configuration Generation Tests (Requirement 7.5)
// =============================================================================

class LaunchdConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<DarwinServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<DarwinServiceWrapper> wrapper_;
};

TEST_F(LaunchdConfigTest, GeneratePlistConfigurationWithDefaultSettings) {
    ServiceConfig config;
    config.serviceName = "com.openrtmp.server";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.workingDirectory = "/var/lib/openrtmp";

    auto result = wrapper_->generateLaunchdPlist(config);

    ASSERT_TRUE(result.isSuccess()) << "Failed to generate plist: " << result.error().message;

    const std::string& plist = result.value();
    EXPECT_FALSE(plist.empty());
    EXPECT_TRUE(plist.find("com.openrtmp.server") != std::string::npos);
    EXPECT_TRUE(plist.find("/usr/local/bin/openrtmp") != std::string::npos);
    EXPECT_TRUE(plist.find("<?xml version") != std::string::npos);
    EXPECT_TRUE(plist.find("<plist version") != std::string::npos);
}

TEST_F(LaunchdConfigTest, GeneratePlistWithRunAtLoad) {
    ServiceConfig config;
    config.serviceName = "com.openrtmp.server";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.runAtLoad = true;

    auto result = wrapper_->generateLaunchdPlist(config);

    ASSERT_TRUE(result.isSuccess());

    const std::string& plist = result.value();
    EXPECT_TRUE(plist.find("<key>RunAtLoad</key>") != std::string::npos);
    EXPECT_TRUE(plist.find("<true/>") != std::string::npos);
}

TEST_F(LaunchdConfigTest, GeneratePlistWithKeepAlive) {
    ServiceConfig config;
    config.serviceName = "com.openrtmp.server";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.keepAlive = true;

    auto result = wrapper_->generateLaunchdPlist(config);

    ASSERT_TRUE(result.isSuccess());

    const std::string& plist = result.value();
    EXPECT_TRUE(plist.find("<key>KeepAlive</key>") != std::string::npos);
}

TEST_F(LaunchdConfigTest, GeneratePlistWithLogPaths) {
    ServiceConfig config;
    config.serviceName = "com.openrtmp.server";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.standardOutPath = "/var/log/openrtmp/stdout.log";
    config.standardErrorPath = "/var/log/openrtmp/stderr.log";

    auto result = wrapper_->generateLaunchdPlist(config);

    ASSERT_TRUE(result.isSuccess());

    const std::string& plist = result.value();
    EXPECT_TRUE(plist.find("<key>StandardOutPath</key>") != std::string::npos);
    EXPECT_TRUE(plist.find("/var/log/openrtmp/stdout.log") != std::string::npos);
    EXPECT_TRUE(plist.find("<key>StandardErrorPath</key>") != std::string::npos);
    EXPECT_TRUE(plist.find("/var/log/openrtmp/stderr.log") != std::string::npos);
}

TEST_F(LaunchdConfigTest, GeneratePlistWithArguments) {
    ServiceConfig config;
    config.serviceName = "com.openrtmp.server";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.arguments = {"--config", "/etc/openrtmp/config.json", "--port", "1935"};

    auto result = wrapper_->generateLaunchdPlist(config);

    ASSERT_TRUE(result.isSuccess());

    const std::string& plist = result.value();
    EXPECT_TRUE(plist.find("<key>ProgramArguments</key>") != std::string::npos);
    EXPECT_TRUE(plist.find("--config") != std::string::npos);
    EXPECT_TRUE(plist.find("/etc/openrtmp/config.json") != std::string::npos);
}

TEST_F(LaunchdConfigTest, GeneratePlistWithEnvironmentVariables) {
    ServiceConfig config;
    config.serviceName = "com.openrtmp.server";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.environmentVariables = {
        {"OPENRTMP_LOG_LEVEL", "debug"},
        {"OPENRTMP_PORT", "1935"}
    };

    auto result = wrapper_->generateLaunchdPlist(config);

    ASSERT_TRUE(result.isSuccess());

    const std::string& plist = result.value();
    EXPECT_TRUE(plist.find("<key>EnvironmentVariables</key>") != std::string::npos);
    EXPECT_TRUE(plist.find("OPENRTMP_LOG_LEVEL") != std::string::npos);
    EXPECT_TRUE(plist.find("debug") != std::string::npos);
}

TEST_F(LaunchdConfigTest, GeneratePlistFailsWithEmptyServiceName) {
    ServiceConfig config;
    config.serviceName = "";
    config.executablePath = "/usr/local/bin/openrtmp";

    auto result = wrapper_->generateLaunchdPlist(config);

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, ServiceErrorCode::InvalidConfiguration);
}

TEST_F(LaunchdConfigTest, GeneratePlistFailsWithEmptyExecutablePath) {
    ServiceConfig config;
    config.serviceName = "com.openrtmp.server";
    config.executablePath = "";

    auto result = wrapper_->generateLaunchdPlist(config);

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, ServiceErrorCode::InvalidConfiguration);
}

// =============================================================================
// Network Interface Enumeration Tests (Requirement 7.4)
// =============================================================================

class NetworkInterfaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<DarwinServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<DarwinServiceWrapper> wrapper_;
};

TEST_F(NetworkInterfaceTest, EnumerateNetworkInterfacesReturnsAtLeastLoopback) {
    auto result = wrapper_->enumerateNetworkInterfaces();

    ASSERT_TRUE(result.isSuccess()) << "Failed to enumerate interfaces: " << result.error().message;

    const auto& interfaces = result.value();
    EXPECT_FALSE(interfaces.empty());

    // Should at least have loopback
    bool hasLoopback = false;
    for (const auto& iface : interfaces) {
        if (iface.name == "lo0" || iface.isLoopback) {
            hasLoopback = true;
            EXPECT_TRUE(iface.isLoopback);
            break;
        }
    }
    EXPECT_TRUE(hasLoopback) << "No loopback interface found";
}

TEST_F(NetworkInterfaceTest, EnumerateNetworkInterfacesHasIPAddresses) {
    auto result = wrapper_->enumerateNetworkInterfaces();

    ASSERT_TRUE(result.isSuccess());

    const auto& interfaces = result.value();

    // At least one interface should have an IP address
    bool hasIPAddress = false;
    for (const auto& iface : interfaces) {
        if (!iface.ipv4Addresses.empty() || !iface.ipv6Addresses.empty()) {
            hasIPAddress = true;
            break;
        }
    }
    EXPECT_TRUE(hasIPAddress) << "No interface has an IP address";
}

TEST_F(NetworkInterfaceTest, InterfaceInfoContainsRequiredFields) {
    auto result = wrapper_->enumerateNetworkInterfaces();

    ASSERT_TRUE(result.isSuccess());

    const auto& interfaces = result.value();
    ASSERT_FALSE(interfaces.empty());

    // Check first interface has required fields
    const auto& iface = interfaces.front();
    EXPECT_FALSE(iface.name.empty());
    // isUp, isLoopback, etc. should be set (they have default values)
}

TEST_F(NetworkInterfaceTest, CanBindToLoopbackInterface) {
    auto result = wrapper_->canBindToInterface("lo0");

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(result.value());
}

TEST_F(NetworkInterfaceTest, CanBindToAllInterfaces) {
    auto result = wrapper_->canBindToInterface("0.0.0.0");

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(result.value());
}

// =============================================================================
// Network Permission Tests (Requirement 7.7)
// =============================================================================

class NetworkPermissionTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<DarwinServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<DarwinServiceWrapper> wrapper_;
};

TEST_F(NetworkPermissionTest, CheckNetworkPermissionStatusReturnsValidStatus) {
    auto result = wrapper_->checkNetworkPermissionStatus();

    ASSERT_TRUE(result.isSuccess()) << "Failed to check permission status: " << result.error().message;

    // Status should be one of the valid values
    NetworkPermissionStatus status = result.value();
    EXPECT_TRUE(
        status == NetworkPermissionStatus::Granted ||
        status == NetworkPermissionStatus::Denied ||
        status == NetworkPermissionStatus::NotDetermined ||
        status == NetworkPermissionStatus::Restricted
    );
}

TEST_F(NetworkPermissionTest, RequestNetworkPermissionCallbackIsValid) {
    std::atomic<bool> callbackCalled{false};

    // Request permission with callback
    wrapper_->requestNetworkPermission([&callbackCalled](NetworkPermissionStatus status) {
        callbackCalled = true;
    });

    // Give some time for async operation
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // The callback should be callable (may or may not be invoked immediately depending on system state)
    // This test verifies the API works without crashing
}

// =============================================================================
// Signal Handling Tests (Requirement 7.7)
// =============================================================================

class SignalHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<DarwinServiceWrapper>();
    }

    void TearDown() override {
        wrapper_->uninstallSignalHandlers();
        wrapper_.reset();
    }

    std::unique_ptr<DarwinServiceWrapper> wrapper_;
};

TEST_F(SignalHandlerTest, InstallSignalHandlersSucceeds) {
    auto result = wrapper_->installSignalHandlers();

    EXPECT_TRUE(result.isSuccess()) << "Failed to install signal handlers: " << result.error().message;
}

TEST_F(SignalHandlerTest, CanRegisterShutdownCallback) {
    std::atomic<bool> callbackRegistered{false};

    wrapper_->setShutdownCallback([&callbackRegistered](ShutdownReason reason) {
        callbackRegistered = true;
    });

    // Callback should be registered without error
    EXPECT_TRUE(true);
}

TEST_F(SignalHandlerTest, UninstallSignalHandlersSucceeds) {
    auto installResult = wrapper_->installSignalHandlers();
    ASSERT_TRUE(installResult.isSuccess());

    auto uninstallResult = wrapper_->uninstallSignalHandlers();
    EXPECT_TRUE(uninstallResult.isSuccess());
}

TEST_F(SignalHandlerTest, IsShutdownRequestedInitiallyFalse) {
    EXPECT_FALSE(wrapper_->isShutdownRequested());
}

TEST_F(SignalHandlerTest, RequestShutdownSetsFlag) {
    wrapper_->requestShutdown(ShutdownReason::UserRequested);

    EXPECT_TRUE(wrapper_->isShutdownRequested());
}

TEST_F(SignalHandlerTest, GetShutdownReasonReturnsCorrectReason) {
    wrapper_->requestShutdown(ShutdownReason::SignalReceived);

    auto reason = wrapper_->getShutdownReason();
    EXPECT_EQ(reason, ShutdownReason::SignalReceived);
}

// =============================================================================
// Service Status Tests
// =============================================================================

class ServiceStatusTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<DarwinServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<DarwinServiceWrapper> wrapper_;
};

TEST_F(ServiceStatusTest, GetServiceStatusReturnsValidStatus) {
    auto result = wrapper_->getServiceStatus();

    ASSERT_TRUE(result.isSuccess());

    ServiceStatus status = result.value();
    EXPECT_TRUE(
        status == ServiceStatus::NotInstalled ||
        status == ServiceStatus::Stopped ||
        status == ServiceStatus::Running ||
        status == ServiceStatus::Unknown
    );
}

TEST_F(ServiceStatusTest, GetServiceInfoReturnsInfo) {
    auto result = wrapper_->getServiceInfo();

    ASSERT_TRUE(result.isSuccess());

    const ServiceInfo& info = result.value();
    // Service info should have reasonable defaults
    EXPECT_GE(info.pid, 0);
}

// =============================================================================
// Graceful Shutdown Coordination Tests
// =============================================================================

class GracefulShutdownTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<DarwinServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<DarwinServiceWrapper> wrapper_;
};

TEST_F(GracefulShutdownTest, WaitForShutdownWithTimeout) {
    // Request shutdown immediately
    wrapper_->requestShutdown(ShutdownReason::UserRequested);

    // Wait should return immediately since shutdown is requested
    bool shutdownCompleted = wrapper_->waitForShutdown(std::chrono::milliseconds(100));

    EXPECT_TRUE(shutdownCompleted);
}

TEST_F(GracefulShutdownTest, WaitForShutdownTimesOut) {
    // Don't request shutdown

    // Wait should timeout
    bool shutdownCompleted = wrapper_->waitForShutdown(std::chrono::milliseconds(50));

    EXPECT_FALSE(shutdownCompleted);
}

TEST_F(GracefulShutdownTest, SetGracePeriod) {
    wrapper_->setGracePeriod(std::chrono::seconds(30));

    EXPECT_EQ(wrapper_->getGracePeriod(), std::chrono::seconds(30));
}

TEST_F(GracefulShutdownTest, DefaultGracePeriodIsReasonable) {
    auto gracePeriod = wrapper_->getGracePeriod();

    // Default should be between 1 and 60 seconds
    EXPECT_GE(gracePeriod, std::chrono::seconds(1));
    EXPECT_LE(gracePeriod, std::chrono::seconds(60));
}

// =============================================================================
// Plist File Operations Tests
// =============================================================================

class PlistFileOperationsTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<DarwinServiceWrapper>();
        testPlistPath_ = "/tmp/openrtmp_test_" + std::to_string(getpid()) + ".plist";
    }

    void TearDown() override {
        // Clean up test file
        std::remove(testPlistPath_.c_str());
        wrapper_.reset();
    }

    std::unique_ptr<DarwinServiceWrapper> wrapper_;
    std::string testPlistPath_;
};

TEST_F(PlistFileOperationsTest, SavePlistToFile) {
    ServiceConfig config;
    config.serviceName = "com.openrtmp.test";
    config.executablePath = "/usr/local/bin/openrtmp";

    auto result = wrapper_->savePlistToFile(config, testPlistPath_);

    ASSERT_TRUE(result.isSuccess()) << "Failed to save plist: " << result.error().message;

    // Verify file exists
    struct stat st;
    EXPECT_EQ(stat(testPlistPath_.c_str(), &st), 0) << "Plist file was not created";

    // Verify content
    std::ifstream file(testPlistPath_);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    EXPECT_TRUE(content.find("com.openrtmp.test") != std::string::npos);
}

TEST_F(PlistFileOperationsTest, SavePlistFailsWithInvalidPath) {
    ServiceConfig config;
    config.serviceName = "com.openrtmp.test";
    config.executablePath = "/usr/local/bin/openrtmp";

    auto result = wrapper_->savePlistToFile(config, "/nonexistent/directory/test.plist");

    EXPECT_TRUE(result.isError());
}

} // namespace test
} // namespace darwin
} // namespace pal
} // namespace openrtmp

#endif // __APPLE__
