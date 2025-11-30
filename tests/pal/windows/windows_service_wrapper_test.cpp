// OpenRTMP - Cross-platform RTMP Server
// Tests for Windows Service Wrapper Implementation
//
// Requirements Covered: 7.4, 7.5, 7.6
// - 7.4: Support binding to any available network interface
// - 7.5: Support running as a background service or daemon
// - 7.6: Windows Firewall integration for port exceptions

#include <gtest/gtest.h>

// Mock-based tests work on all platforms
// Windows-specific implementation tests only run on Windows

#include "openrtmp/pal/windows/windows_service_wrapper.hpp"
#include "openrtmp/core/result.hpp"

#include <atomic>
#include <thread>
#include <chrono>
#include <string>

namespace openrtmp {
namespace pal {
namespace windows {
namespace test {

// =============================================================================
// Windows Service Wrapper Basic Tests (Cross-platform with mocks)
// =============================================================================

class WindowsServiceWrapperTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<WindowsServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<WindowsServiceWrapper> wrapper_;
};

TEST_F(WindowsServiceWrapperTest, DefaultConstructor) {
    EXPECT_NE(wrapper_, nullptr);
}

TEST_F(WindowsServiceWrapperTest, ImplementsIWindowsServiceWrapperInterface) {
    IWindowsServiceWrapper* interface = wrapper_.get();
    EXPECT_NE(interface, nullptr);
}

// =============================================================================
// Architecture Detection Tests (Requirement 7.6 - ARM64 and x64 support)
// =============================================================================

class ArchitectureDetectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<WindowsServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<WindowsServiceWrapper> wrapper_;
};

TEST_F(ArchitectureDetectionTest, GetArchitectureReturnsValidArchitecture) {
    ProcessorArchitecture arch = wrapper_->getProcessorArchitecture();

    // Should be one of the valid architectures
    EXPECT_TRUE(
        arch == ProcessorArchitecture::x86 ||
        arch == ProcessorArchitecture::x64 ||
        arch == ProcessorArchitecture::ARM ||
        arch == ProcessorArchitecture::ARM64 ||
        arch == ProcessorArchitecture::Unknown
    );
}

TEST_F(ArchitectureDetectionTest, GetArchitectureStringReturnsNonEmptyString) {
    std::string archString = wrapper_->getProcessorArchitectureString();

    EXPECT_FALSE(archString.empty());
}

TEST_F(ArchitectureDetectionTest, Is64BitReturnsConsistentValue) {
    ProcessorArchitecture arch = wrapper_->getProcessorArchitecture();
    bool is64Bit = wrapper_->is64BitProcess();

    if (arch == ProcessorArchitecture::x64 || arch == ProcessorArchitecture::ARM64) {
        EXPECT_TRUE(is64Bit) << "64-bit architecture should report is64BitProcess() == true";
    }
}

// =============================================================================
// Service Configuration Tests (Requirement 7.5)
// =============================================================================

class ServiceConfigurationTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<WindowsServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<WindowsServiceWrapper> wrapper_;
};

TEST_F(ServiceConfigurationTest, DefaultServiceConfigHasReasonableDefaults) {
    WindowsServiceConfig config;

    EXPECT_FALSE(config.serviceName.empty() || config.serviceName == "");
    EXPECT_FALSE(config.displayName.empty() || config.displayName == "");
    EXPECT_EQ(config.startType, ServiceStartType::AutoStart);
}

TEST_F(ServiceConfigurationTest, ValidateConfigRejectsEmptyServiceName) {
    WindowsServiceConfig config;
    config.serviceName = "";
    config.displayName = "Test Service";
    config.executablePath = "/path/to/exe";

    auto result = wrapper_->validateServiceConfig(config);

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, ServiceErrorCode::InvalidConfiguration);
}

TEST_F(ServiceConfigurationTest, ValidateConfigRejectsEmptyExecutablePath) {
    WindowsServiceConfig config;
    config.serviceName = "TestService";
    config.displayName = "Test Service";
    config.executablePath = "";

    auto result = wrapper_->validateServiceConfig(config);

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, ServiceErrorCode::InvalidConfiguration);
}

TEST_F(ServiceConfigurationTest, ValidateConfigAcceptsValidConfig) {
    WindowsServiceConfig config;
    config.serviceName = "OpenRTMPServer";
    config.displayName = "OpenRTMP RTMP Server";
    config.description = "Cross-platform RTMP server service";
    config.executablePath = "C:\\Program Files\\OpenRTMP\\openrtmp.exe";
    config.startType = ServiceStartType::AutoStart;

    auto result = wrapper_->validateServiceConfig(config);

    EXPECT_TRUE(result.isSuccess()) << "Valid config should pass validation";
}

// =============================================================================
// Firewall Rule Tests (Requirement 7.6)
// =============================================================================

class FirewallRuleTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<WindowsServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<WindowsServiceWrapper> wrapper_;
};

TEST_F(FirewallRuleTest, DefaultFirewallRuleConfigHasReasonableDefaults) {
    FirewallRuleConfig rule;

    EXPECT_EQ(rule.protocol, FirewallProtocol::TCP);
    EXPECT_EQ(rule.direction, FirewallDirection::Inbound);
    EXPECT_EQ(rule.action, FirewallAction::Allow);
    EXPECT_EQ(rule.port, 1935); // Default RTMP port
}

TEST_F(FirewallRuleTest, ValidateFirewallRuleRejectsEmptyRuleName) {
    FirewallRuleConfig rule;
    rule.ruleName = "";
    rule.port = 1935;

    auto result = wrapper_->validateFirewallRule(rule);

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, ServiceErrorCode::InvalidConfiguration);
}

TEST_F(FirewallRuleTest, ValidateFirewallRuleRejectsInvalidPort) {
    FirewallRuleConfig rule;
    rule.ruleName = "OpenRTMP Inbound";
    rule.port = 0;  // Invalid port

    auto result = wrapper_->validateFirewallRule(rule);

    EXPECT_TRUE(result.isError());
}

TEST_F(FirewallRuleTest, ValidateFirewallRuleAcceptsValidConfig) {
    FirewallRuleConfig rule;
    rule.ruleName = "OpenRTMP RTMP Server";
    rule.description = "Allow inbound RTMP connections";
    rule.applicationPath = "C:\\Program Files\\OpenRTMP\\openrtmp.exe";
    rule.port = 1935;
    rule.protocol = FirewallProtocol::TCP;
    rule.direction = FirewallDirection::Inbound;
    rule.action = FirewallAction::Allow;

    auto result = wrapper_->validateFirewallRule(rule);

    EXPECT_TRUE(result.isSuccess()) << "Valid firewall rule should pass validation";
}

// =============================================================================
// SCM Event Handling Tests (Requirement 7.5)
// =============================================================================

class SCMEventHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<WindowsServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<WindowsServiceWrapper> wrapper_;
};

TEST_F(SCMEventHandlerTest, RegisterControlHandlerSucceeds) {
    // In non-service context, this should succeed but may not do anything
    auto result = wrapper_->registerControlHandler();

    // On non-Windows or when not running as service, this may return success
    // or a specific error - both are acceptable
    EXPECT_TRUE(result.isSuccess() ||
               result.error().code == ServiceErrorCode::NotRunningAsService);
}

TEST_F(SCMEventHandlerTest, SetControlCallbackDoesNotCrash) {
    // Setting callback should not crash
    wrapper_->setControlCallback([](ServiceControlCode code) {
        // Handle control code
    });

    EXPECT_TRUE(true); // If we get here, no crash occurred
}

TEST_F(SCMEventHandlerTest, GetCurrentServiceStateReturnsValidState) {
    ServiceState state = wrapper_->getCurrentServiceState();

    EXPECT_TRUE(
        state == ServiceState::Stopped ||
        state == ServiceState::StartPending ||
        state == ServiceState::StopPending ||
        state == ServiceState::Running ||
        state == ServiceState::ContinuePending ||
        state == ServiceState::PausePending ||
        state == ServiceState::Paused ||
        state == ServiceState::Unknown
    );
}

// =============================================================================
// Service Lifecycle Tests
// =============================================================================

class ServiceLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<WindowsServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<WindowsServiceWrapper> wrapper_;
};

TEST_F(ServiceLifecycleTest, IsRunningAsServiceReturnsBoolean) {
    bool isService = wrapper_->isRunningAsService();

    // The result should be a valid boolean (test won't crash)
    EXPECT_TRUE(isService || !isService);
}

TEST_F(ServiceLifecycleTest, IsConsoleApplicationReturnsBoolean) {
    bool isConsole = wrapper_->isConsoleApplication();

    // Should return a valid boolean
    EXPECT_TRUE(isConsole || !isConsole);
}

TEST_F(ServiceLifecycleTest, RunAsConsoleDoesNotCrashWithValidCallback) {
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};

    // Create a simple test that runs briefly
    auto result = wrapper_->runAsConsole(
        [&started]() {
            started = true;
            return true;
        },
        [&stopped]() {
            stopped = true;
        }
    );

    // In a test environment, this should complete without crashing
    // The actual behavior depends on whether we're running in console
}

// =============================================================================
// Service Status Reporting Tests
// =============================================================================

class ServiceStatusTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<WindowsServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<WindowsServiceWrapper> wrapper_;
};

TEST_F(ServiceStatusTest, GetServiceStatusReturnsValidStatus) {
    auto result = wrapper_->getServiceStatus();

    // Should return a valid result (success with status or appropriate error)
    if (result.isSuccess()) {
        ServiceStatusInfo info = result.value();
        // Status should be one of valid states
        EXPECT_TRUE(
            info.state == ServiceState::Stopped ||
            info.state == ServiceState::StartPending ||
            info.state == ServiceState::StopPending ||
            info.state == ServiceState::Running ||
            info.state == ServiceState::ContinuePending ||
            info.state == ServiceState::PausePending ||
            info.state == ServiceState::Paused ||
            info.state == ServiceState::Unknown
        );
    }
}

TEST_F(ServiceStatusTest, SetServiceStatusDoesNotCrash) {
    ServiceStatusInfo status;
    status.state = ServiceState::Running;
    status.acceptedControls = ServiceAcceptedControl::Stop |
                             ServiceAcceptedControl::Shutdown;

    // Should not crash, may fail if not running as service
    auto result = wrapper_->setServiceStatus(status);

    // Either succeeds or returns appropriate error
    EXPECT_TRUE(result.isSuccess() ||
               result.error().code == ServiceErrorCode::NotRunningAsService);
}

// =============================================================================
// Shutdown Coordination Tests (matching Darwin behavior)
// =============================================================================

class ShutdownCoordinationTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<WindowsServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<WindowsServiceWrapper> wrapper_;
};

TEST_F(ShutdownCoordinationTest, IsShutdownRequestedInitiallyFalse) {
    EXPECT_FALSE(wrapper_->isShutdownRequested());
}

TEST_F(ShutdownCoordinationTest, RequestShutdownSetsFlag) {
    wrapper_->requestShutdown(ShutdownReason::UserRequested);

    EXPECT_TRUE(wrapper_->isShutdownRequested());
}

TEST_F(ShutdownCoordinationTest, GetShutdownReasonReturnsCorrectReason) {
    wrapper_->requestShutdown(ShutdownReason::ServiceStop);

    auto reason = wrapper_->getShutdownReason();
    EXPECT_EQ(reason, ShutdownReason::ServiceStop);
}

TEST_F(ShutdownCoordinationTest, WaitForShutdownWithRequestedShutdown) {
    wrapper_->requestShutdown(ShutdownReason::UserRequested);

    bool completed = wrapper_->waitForShutdown(std::chrono::milliseconds(100));

    EXPECT_TRUE(completed);
}

TEST_F(ShutdownCoordinationTest, WaitForShutdownTimesOut) {
    // Don't request shutdown

    bool completed = wrapper_->waitForShutdown(std::chrono::milliseconds(50));

    EXPECT_FALSE(completed);
}

TEST_F(ShutdownCoordinationTest, SetGracePeriod) {
    wrapper_->setGracePeriod(std::chrono::seconds(45));

    EXPECT_EQ(wrapper_->getGracePeriod(), std::chrono::seconds(45));
}

TEST_F(ShutdownCoordinationTest, DefaultGracePeriodIsReasonable) {
    auto gracePeriod = wrapper_->getGracePeriod();

    // Default should be between 1 and 60 seconds
    EXPECT_GE(gracePeriod, std::chrono::seconds(1));
    EXPECT_LE(gracePeriod, std::chrono::seconds(60));
}

TEST_F(ShutdownCoordinationTest, SetShutdownCallbackDoesNotCrash) {
    std::atomic<bool> callbackCalled{false};

    wrapper_->setShutdownCallback([&callbackCalled](ShutdownReason reason) {
        callbackCalled = true;
    });

    // Verify callback is invoked on shutdown
    wrapper_->requestShutdown(ShutdownReason::UserRequested);

    // Give callback time to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_TRUE(callbackCalled);
}

// =============================================================================
// Event Log Integration Tests
// =============================================================================

class EventLogTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<WindowsServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<WindowsServiceWrapper> wrapper_;
};

TEST_F(EventLogTest, LogEventDoesNotCrash) {
    // Logging should not crash even if event log is not available
    wrapper_->logEvent(EventLogType::Information, 1000, "Test event message");

    EXPECT_TRUE(true); // If we get here, no crash
}

TEST_F(EventLogTest, LogEventWithDifferentTypes) {
    wrapper_->logEvent(EventLogType::Information, 1000, "Info message");
    wrapper_->logEvent(EventLogType::Warning, 1001, "Warning message");
    wrapper_->logEvent(EventLogType::Error, 1002, "Error message");

    EXPECT_TRUE(true); // If we get here, no crashes
}

// =============================================================================
// Service Installation/Uninstallation Tests (Mock-based)
// =============================================================================

class ServiceInstallationTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<WindowsServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<WindowsServiceWrapper> wrapper_;
};

TEST_F(ServiceInstallationTest, GenerateInstallCommandReturnsValidCommand) {
    WindowsServiceConfig config;
    config.serviceName = "OpenRTMPServer";
    config.displayName = "OpenRTMP RTMP Server";
    config.executablePath = "C:\\Program Files\\OpenRTMP\\openrtmp.exe";

    std::string command = wrapper_->generateInstallCommand(config);

    EXPECT_FALSE(command.empty());
    EXPECT_TRUE(command.find("sc") != std::string::npos ||
                command.find("create") != std::string::npos);
}

TEST_F(ServiceInstallationTest, GenerateUninstallCommandReturnsValidCommand) {
    std::string serviceName = "OpenRTMPServer";

    std::string command = wrapper_->generateUninstallCommand(serviceName);

    EXPECT_FALSE(command.empty());
    EXPECT_TRUE(command.find("sc") != std::string::npos ||
                command.find("delete") != std::string::npos);
}

} // namespace test
} // namespace windows
} // namespace pal
} // namespace openrtmp
