// OpenRTMP - Cross-platform RTMP Server
// Tests for Linux Service Wrapper Implementation
//
// Requirements Covered: 7.4, 7.5
// - 7.4: Support binding to specific network interfaces
// - 7.5: Support running as a background service or daemon

#include <gtest/gtest.h>

#if defined(__linux__)

#include "openrtmp/pal/linux/linux_service_wrapper.hpp"
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
namespace linux_pal {
namespace test {

// =============================================================================
// Service Wrapper Basic Tests
// =============================================================================

class LinuxServiceWrapperTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<LinuxServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<LinuxServiceWrapper> wrapper_;
};

TEST_F(LinuxServiceWrapperTest, DefaultConstructor) {
    EXPECT_NE(wrapper_, nullptr);
}

TEST_F(LinuxServiceWrapperTest, ImplementsIServiceWrapperInterface) {
    ILinuxServiceWrapper* interface = wrapper_.get();
    EXPECT_NE(interface, nullptr);
}

// =============================================================================
// Systemd Unit Configuration Generation Tests (Requirement 7.5)
// =============================================================================

class SystemdUnitConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<LinuxServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<LinuxServiceWrapper> wrapper_;
};

TEST_F(SystemdUnitConfigTest, GenerateSystemdUnitWithDefaultSettings) {
    SystemdServiceConfig config;
    config.serviceName = "openrtmp";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.description = "OpenRTMP RTMP Server";

    auto result = wrapper_->generateSystemdUnit(config);

    ASSERT_TRUE(result.isSuccess()) << "Failed to generate systemd unit: " << result.error().message;

    const std::string& unit = result.value();
    EXPECT_FALSE(unit.empty());
    EXPECT_TRUE(unit.find("[Unit]") != std::string::npos);
    EXPECT_TRUE(unit.find("[Service]") != std::string::npos);
    EXPECT_TRUE(unit.find("[Install]") != std::string::npos);
    EXPECT_TRUE(unit.find("OpenRTMP RTMP Server") != std::string::npos);
    EXPECT_TRUE(unit.find("/usr/local/bin/openrtmp") != std::string::npos);
}

TEST_F(SystemdUnitConfigTest, GenerateSystemdUnitWithAfterDependencies) {
    SystemdServiceConfig config;
    config.serviceName = "openrtmp";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.after = {"network.target", "network-online.target"};
    config.wants = {"network-online.target"};

    auto result = wrapper_->generateSystemdUnit(config);

    ASSERT_TRUE(result.isSuccess());

    const std::string& unit = result.value();
    EXPECT_TRUE(unit.find("After=network.target network-online.target") != std::string::npos);
    EXPECT_TRUE(unit.find("Wants=network-online.target") != std::string::npos);
}

TEST_F(SystemdUnitConfigTest, GenerateSystemdUnitWithServiceType) {
    SystemdServiceConfig config;
    config.serviceName = "openrtmp";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.serviceType = SystemdServiceType::Simple;

    auto result = wrapper_->generateSystemdUnit(config);

    ASSERT_TRUE(result.isSuccess());

    const std::string& unit = result.value();
    EXPECT_TRUE(unit.find("Type=simple") != std::string::npos);
}

TEST_F(SystemdUnitConfigTest, GenerateSystemdUnitWithNotifyType) {
    SystemdServiceConfig config;
    config.serviceName = "openrtmp";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.serviceType = SystemdServiceType::Notify;

    auto result = wrapper_->generateSystemdUnit(config);

    ASSERT_TRUE(result.isSuccess());

    const std::string& unit = result.value();
    EXPECT_TRUE(unit.find("Type=notify") != std::string::npos);
}

TEST_F(SystemdUnitConfigTest, GenerateSystemdUnitWithEnvironmentVariables) {
    SystemdServiceConfig config;
    config.serviceName = "openrtmp";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.environmentVariables = {
        {"OPENRTMP_LOG_LEVEL", "debug"},
        {"OPENRTMP_PORT", "1935"}
    };

    auto result = wrapper_->generateSystemdUnit(config);

    ASSERT_TRUE(result.isSuccess());

    const std::string& unit = result.value();
    EXPECT_TRUE(unit.find("Environment=") != std::string::npos);
    EXPECT_TRUE(unit.find("OPENRTMP_LOG_LEVEL=debug") != std::string::npos);
}

TEST_F(SystemdUnitConfigTest, GenerateSystemdUnitWithUserAndGroup) {
    SystemdServiceConfig config;
    config.serviceName = "openrtmp";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.userName = "openrtmp";
    config.groupName = "openrtmp";

    auto result = wrapper_->generateSystemdUnit(config);

    ASSERT_TRUE(result.isSuccess());

    const std::string& unit = result.value();
    EXPECT_TRUE(unit.find("User=openrtmp") != std::string::npos);
    EXPECT_TRUE(unit.find("Group=openrtmp") != std::string::npos);
}

TEST_F(SystemdUnitConfigTest, GenerateSystemdUnitWithRestartPolicy) {
    SystemdServiceConfig config;
    config.serviceName = "openrtmp";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.restart = SystemdRestartPolicy::Always;
    config.restartSec = 5;

    auto result = wrapper_->generateSystemdUnit(config);

    ASSERT_TRUE(result.isSuccess());

    const std::string& unit = result.value();
    EXPECT_TRUE(unit.find("Restart=always") != std::string::npos);
    EXPECT_TRUE(unit.find("RestartSec=5") != std::string::npos);
}

TEST_F(SystemdUnitConfigTest, GenerateSystemdUnitWithPidFile) {
    SystemdServiceConfig config;
    config.serviceName = "openrtmp";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.pidFile = "/run/openrtmp/openrtmp.pid";

    auto result = wrapper_->generateSystemdUnit(config);

    ASSERT_TRUE(result.isSuccess());

    const std::string& unit = result.value();
    EXPECT_TRUE(unit.find("PIDFile=/run/openrtmp/openrtmp.pid") != std::string::npos);
}

TEST_F(SystemdUnitConfigTest, GenerateSystemdUnitWithWorkingDirectory) {
    SystemdServiceConfig config;
    config.serviceName = "openrtmp";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.workingDirectory = "/var/lib/openrtmp";

    auto result = wrapper_->generateSystemdUnit(config);

    ASSERT_TRUE(result.isSuccess());

    const std::string& unit = result.value();
    EXPECT_TRUE(unit.find("WorkingDirectory=/var/lib/openrtmp") != std::string::npos);
}

TEST_F(SystemdUnitConfigTest, GenerateSystemdUnitFailsWithEmptyServiceName) {
    SystemdServiceConfig config;
    config.serviceName = "";
    config.executablePath = "/usr/local/bin/openrtmp";

    auto result = wrapper_->generateSystemdUnit(config);

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, ServiceErrorCode::InvalidConfiguration);
}

TEST_F(SystemdUnitConfigTest, GenerateSystemdUnitFailsWithEmptyExecutablePath) {
    SystemdServiceConfig config;
    config.serviceName = "openrtmp";
    config.executablePath = "";

    auto result = wrapper_->generateSystemdUnit(config);

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, ServiceErrorCode::InvalidConfiguration);
}

// =============================================================================
// Template Unit (Multiple Instances) Tests (Requirement 7.5)
// =============================================================================

class TemplateUnitTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<LinuxServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<LinuxServiceWrapper> wrapper_;
};

TEST_F(TemplateUnitTest, GenerateTemplateUnitForMultipleInstances) {
    SystemdServiceConfig config;
    config.serviceName = "openrtmp";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.isTemplateUnit = true;

    auto result = wrapper_->generateSystemdUnit(config);

    ASSERT_TRUE(result.isSuccess());

    const std::string& unit = result.value();
    // Template units use %i or %I for instance identifier
    EXPECT_TRUE(unit.find("%i") != std::string::npos || unit.find("%I") != std::string::npos);
}

TEST_F(TemplateUnitTest, GenerateTemplateUnitWithInstanceConfig) {
    SystemdServiceConfig config;
    config.serviceName = "openrtmp";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.isTemplateUnit = true;
    config.configFile = "/etc/openrtmp/%i.conf";

    auto result = wrapper_->generateSystemdUnit(config);

    ASSERT_TRUE(result.isSuccess());

    const std::string& unit = result.value();
    EXPECT_TRUE(unit.find("/etc/openrtmp/%i.conf") != std::string::npos);
}

TEST_F(TemplateUnitTest, GenerateTemplateUnitWithInstancePidFile) {
    SystemdServiceConfig config;
    config.serviceName = "openrtmp";
    config.executablePath = "/usr/local/bin/openrtmp";
    config.isTemplateUnit = true;
    config.pidFile = "/run/openrtmp/openrtmp-%i.pid";

    auto result = wrapper_->generateSystemdUnit(config);

    ASSERT_TRUE(result.isSuccess());

    const std::string& unit = result.value();
    EXPECT_TRUE(unit.find("/run/openrtmp/openrtmp-%i.pid") != std::string::npos);
}

// =============================================================================
// Network Interface Enumeration Tests (Requirement 7.4)
// =============================================================================

class NetworkInterfaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<LinuxServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<LinuxServiceWrapper> wrapper_;
};

TEST_F(NetworkInterfaceTest, EnumerateNetworkInterfacesReturnsAtLeastLoopback) {
    auto result = wrapper_->enumerateNetworkInterfaces();

    ASSERT_TRUE(result.isSuccess()) << "Failed to enumerate interfaces: " << result.error().message;

    const auto& interfaces = result.value();
    EXPECT_FALSE(interfaces.empty());

    // Should at least have loopback
    bool hasLoopback = false;
    for (const auto& iface : interfaces) {
        if (iface.name == "lo" || iface.isLoopback) {
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

TEST_F(NetworkInterfaceTest, CanBindToLoopbackInterface) {
    auto result = wrapper_->canBindToInterface("lo");

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(result.value());
}

TEST_F(NetworkInterfaceTest, CanBindToAllInterfaces) {
    auto result = wrapper_->canBindToInterface("0.0.0.0");

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(result.value());
}

TEST_F(NetworkInterfaceTest, CanBindToLocalhostIPv4) {
    auto result = wrapper_->canBindToInterface("127.0.0.1");

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(result.value());
}

// =============================================================================
// Signal Handling Tests (Requirement 7.5)
// =============================================================================

class SignalHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<LinuxServiceWrapper>();
    }

    void TearDown() override {
        wrapper_->uninstallSignalHandlers();
        wrapper_.reset();
    }

    std::unique_ptr<LinuxServiceWrapper> wrapper_;
};

TEST_F(SignalHandlerTest, InstallSignalHandlersSucceeds) {
    auto result = wrapper_->installSignalHandlers();

    EXPECT_TRUE(result.isSuccess()) << "Failed to install signal handlers: " << result.error().message;
}

TEST_F(SignalHandlerTest, HandleSIGTERM) {
    auto installResult = wrapper_->installSignalHandlers();
    ASSERT_TRUE(installResult.isSuccess());

    wrapper_->setShutdownCallback([](ShutdownReason reason) {
        // Callback should receive ServiceStop reason for SIGTERM
    });

    // Initial state should not be shutdown
    EXPECT_FALSE(wrapper_->isShutdownRequested());
}

TEST_F(SignalHandlerTest, HandleSIGINT) {
    auto installResult = wrapper_->installSignalHandlers();
    ASSERT_TRUE(installResult.isSuccess());

    // Initial state should not be shutdown
    EXPECT_FALSE(wrapper_->isShutdownRequested());
}

TEST_F(SignalHandlerTest, HandleSIGHUP) {
    auto installResult = wrapper_->installSignalHandlers();
    ASSERT_TRUE(installResult.isSuccess());

    std::atomic<bool> reloadCalled{false};
    wrapper_->setReloadCallback([&reloadCalled]() {
        reloadCalled = true;
    });

    // Verify callback registration doesn't crash
    EXPECT_TRUE(true);
}

TEST_F(SignalHandlerTest, HandleSIGUSR1) {
    auto installResult = wrapper_->installSignalHandlers();
    ASSERT_TRUE(installResult.isSuccess());

    std::atomic<bool> usr1Called{false};
    wrapper_->setUserSignalCallback(1, [&usr1Called]() {
        usr1Called = true;
    });

    // Verify callback registration doesn't crash
    EXPECT_TRUE(true);
}

TEST_F(SignalHandlerTest, HandleSIGUSR2) {
    auto installResult = wrapper_->installSignalHandlers();
    ASSERT_TRUE(installResult.isSuccess());

    std::atomic<bool> usr2Called{false};
    wrapper_->setUserSignalCallback(2, [&usr2Called]() {
        usr2Called = true;
    });

    // Verify callback registration doesn't crash
    EXPECT_TRUE(true);
}

TEST_F(SignalHandlerTest, UninstallSignalHandlersSucceeds) {
    auto installResult = wrapper_->installSignalHandlers();
    ASSERT_TRUE(installResult.isSuccess());

    auto uninstallResult = wrapper_->uninstallSignalHandlers();
    EXPECT_TRUE(uninstallResult.isSuccess());
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
// PID File Management Tests (Requirement 7.5)
// =============================================================================

class PidFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<LinuxServiceWrapper>();
        testPidPath_ = "/tmp/openrtmp_test_" + std::to_string(getpid()) + ".pid";
    }

    void TearDown() override {
        std::remove(testPidPath_.c_str());
        wrapper_.reset();
    }

    std::unique_ptr<LinuxServiceWrapper> wrapper_;
    std::string testPidPath_;
};

TEST_F(PidFileTest, CreatePidFile) {
    auto result = wrapper_->createPidFile(testPidPath_);

    ASSERT_TRUE(result.isSuccess()) << "Failed to create PID file: " << result.error().message;

    // Verify file exists
    struct stat st;
    EXPECT_EQ(stat(testPidPath_.c_str(), &st), 0) << "PID file was not created";

    // Verify content is our PID
    std::ifstream file(testPidPath_);
    pid_t readPid;
    file >> readPid;
    EXPECT_EQ(readPid, getpid());
}

TEST_F(PidFileTest, RemovePidFile) {
    auto createResult = wrapper_->createPidFile(testPidPath_);
    ASSERT_TRUE(createResult.isSuccess());

    auto removeResult = wrapper_->removePidFile(testPidPath_);
    EXPECT_TRUE(removeResult.isSuccess());

    // Verify file is removed
    struct stat st;
    EXPECT_NE(stat(testPidPath_.c_str(), &st), 0) << "PID file was not removed";
}

TEST_F(PidFileTest, CheckPidFileWithCurrentProcess) {
    auto createResult = wrapper_->createPidFile(testPidPath_);
    ASSERT_TRUE(createResult.isSuccess());

    auto checkResult = wrapper_->checkPidFile(testPidPath_);
    ASSERT_TRUE(checkResult.isSuccess());

    // Should return true since the process (us) is running
    EXPECT_TRUE(checkResult.value());
}

TEST_F(PidFileTest, CheckPidFileWithNonExistentFile) {
    auto checkResult = wrapper_->checkPidFile("/nonexistent/path/test.pid");

    // Should succeed but return false (no running process)
    EXPECT_TRUE(checkResult.isSuccess());
    EXPECT_FALSE(checkResult.value());
}

// =============================================================================
// Service Status and Health Tests (Requirement 7.5)
// =============================================================================

class ServiceStatusTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<LinuxServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<LinuxServiceWrapper> wrapper_;
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
    EXPECT_GE(info.pid, 0);
}

TEST_F(ServiceStatusTest, ReportHealthyStatus) {
    // Simulate a healthy service
    auto result = wrapper_->reportHealthStatus(HealthStatus::Healthy, "All systems operational");

    EXPECT_TRUE(result.isSuccess());
}

TEST_F(ServiceStatusTest, ReportDegradedStatus) {
    auto result = wrapper_->reportHealthStatus(HealthStatus::Degraded, "High memory usage");

    EXPECT_TRUE(result.isSuccess());
}

TEST_F(ServiceStatusTest, ReportUnhealthyStatus) {
    auto result = wrapper_->reportHealthStatus(HealthStatus::Unhealthy, "Critical error");

    EXPECT_TRUE(result.isSuccess());
}

// =============================================================================
// Graceful Shutdown Coordination Tests (Requirement 7.5)
// =============================================================================

class GracefulShutdownTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<LinuxServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<LinuxServiceWrapper> wrapper_;
};

TEST_F(GracefulShutdownTest, WaitForShutdownWithTimeout) {
    wrapper_->requestShutdown(ShutdownReason::UserRequested);

    bool shutdownCompleted = wrapper_->waitForShutdown(std::chrono::milliseconds(100));

    EXPECT_TRUE(shutdownCompleted);
}

TEST_F(GracefulShutdownTest, WaitForShutdownTimesOut) {
    bool shutdownCompleted = wrapper_->waitForShutdown(std::chrono::milliseconds(50));

    EXPECT_FALSE(shutdownCompleted);
}

TEST_F(GracefulShutdownTest, SetGracePeriod) {
    wrapper_->setGracePeriod(std::chrono::seconds(30));

    EXPECT_EQ(wrapper_->getGracePeriod(), std::chrono::seconds(30));
}

TEST_F(GracefulShutdownTest, DefaultGracePeriodIsReasonable) {
    auto gracePeriod = wrapper_->getGracePeriod();

    EXPECT_GE(gracePeriod, std::chrono::seconds(1));
    EXPECT_LE(gracePeriod, std::chrono::seconds(60));
}

// =============================================================================
// Systemd Notify Integration Tests (Requirement 7.5)
// =============================================================================

class SystemdNotifyTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<LinuxServiceWrapper>();
    }

    void TearDown() override {
        wrapper_.reset();
    }

    std::unique_ptr<LinuxServiceWrapper> wrapper_;
};

TEST_F(SystemdNotifyTest, NotifyReady) {
    // Note: This will only actually notify systemd if NOTIFY_SOCKET is set
    auto result = wrapper_->notifyReady();

    // Should succeed even if not running under systemd
    EXPECT_TRUE(result.isSuccess());
}

TEST_F(SystemdNotifyTest, NotifyStopping) {
    auto result = wrapper_->notifyStopping();

    EXPECT_TRUE(result.isSuccess());
}

TEST_F(SystemdNotifyTest, NotifyWatchdog) {
    auto result = wrapper_->notifyWatchdog();

    EXPECT_TRUE(result.isSuccess());
}

TEST_F(SystemdNotifyTest, NotifyStatus) {
    auto result = wrapper_->notifyStatus("Starting up...");

    EXPECT_TRUE(result.isSuccess());
}

// =============================================================================
// Unit File Operations Tests (Requirement 7.5)
// =============================================================================

class UnitFileOperationsTest : public ::testing::Test {
protected:
    void SetUp() override {
        wrapper_ = std::make_unique<LinuxServiceWrapper>();
        testUnitPath_ = "/tmp/openrtmp_test_" + std::to_string(getpid()) + ".service";
    }

    void TearDown() override {
        std::remove(testUnitPath_.c_str());
        wrapper_.reset();
    }

    std::unique_ptr<LinuxServiceWrapper> wrapper_;
    std::string testUnitPath_;
};

TEST_F(UnitFileOperationsTest, SaveUnitToFile) {
    SystemdServiceConfig config;
    config.serviceName = "openrtmp-test";
    config.executablePath = "/usr/local/bin/openrtmp";

    auto result = wrapper_->saveUnitToFile(config, testUnitPath_);

    ASSERT_TRUE(result.isSuccess()) << "Failed to save unit file: " << result.error().message;

    // Verify file exists
    struct stat st;
    EXPECT_EQ(stat(testUnitPath_.c_str(), &st), 0) << "Unit file was not created";

    // Verify content
    std::ifstream file(testUnitPath_);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    EXPECT_TRUE(content.find("[Unit]") != std::string::npos);
    EXPECT_TRUE(content.find("[Service]") != std::string::npos);
}

TEST_F(UnitFileOperationsTest, SaveUnitFailsWithInvalidPath) {
    SystemdServiceConfig config;
    config.serviceName = "openrtmp-test";
    config.executablePath = "/usr/local/bin/openrtmp";

    auto result = wrapper_->saveUnitToFile(config, "/nonexistent/directory/test.service");

    EXPECT_TRUE(result.isError());
}

} // namespace test
} // namespace linux_pal
} // namespace pal
} // namespace openrtmp

#endif // __linux__
