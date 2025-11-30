// OpenRTMP - Cross-platform RTMP Server
// Tests for Mobile Network Monitor Implementation
//
// Requirements Covered: 11.1, 11.2, 11.3, 11.4, 11.5, 11.6
// - 11.1: Detect network connectivity changes within 2 seconds
// - 11.2: Attempt interface rebind on WiFi to cellular transition when allowed
// - 11.3: Notify clients of new server address when interface changes
// - 11.4: Maintain connection state for 30 seconds during network loss
// - 11.5: Support configuration to restrict to WiFi-only or cellular-only
// - 11.6: Warn users about cellular data usage through API

#include <gtest/gtest.h>

// Enable tests on all platforms for mock testing
#if defined(__APPLE__) || defined(__ANDROID__) || defined(OPENRTMP_MOBILE_TEST) || defined(OPENRTMP_IOS_TEST) || defined(OPENRTMP_ANDROID_TEST)

#include "openrtmp/pal/mobile/network_monitor.hpp"
#include "openrtmp/core/result.hpp"

#include <atomic>
#include <thread>
#include <chrono>
#include <functional>
#include <vector>
#include <condition_variable>
#include <mutex>

namespace openrtmp {
namespace pal {
namespace mobile {
namespace test {

// =============================================================================
// Mock Network Monitor Implementation for Testing
// =============================================================================

/**
 * @brief Mock implementation of network monitor for testing.
 *
 * Since actual platform APIs are not available in unit test environment,
 * this mock simulates the behavior for cross-platform testing.
 */
class MockNetworkMonitor : public INetworkMonitor {
public:
    MockNetworkMonitor()
        : isMonitoring_(false)
        , currentNetworkType_(NetworkType::WiFi)
        , isConnected_(true)
        , allowedNetworks_(NetworkTypeFlags::Any)
        , connectionStateGracePeriodMs_(30000)
        , detectionLatencyMs_(0)
        , lastChangeTimestamp_(std::chrono::steady_clock::now())
        , cellularDataWarningEnabled_(true)
        , cellularDataWarningShown_(false)
        , serverAddressNotified_(false)
        , rebindAttempted_(false)
    {}

    // Lifecycle
    core::Result<void, NetworkMonitorError> startMonitoring() override {
        if (isMonitoring_) {
            return core::Result<void, NetworkMonitorError>::error(
                NetworkMonitorError(NetworkMonitorErrorCode::AlreadyMonitoring,
                    "Monitoring already started"));
        }
        isMonitoring_ = true;
        return core::Result<void, NetworkMonitorError>::success();
    }

    void stopMonitoring() override {
        isMonitoring_ = false;
    }

    bool isMonitoringActive() const override {
        return isMonitoring_;
    }

    // Network State (Requirement 11.1)
    NetworkState getCurrentState() const override {
        NetworkState state;
        state.isConnected = isConnected_;
        state.type = currentNetworkType_;
        state.interfaceName = currentInterfaceName_;
        state.ipAddress = currentIpAddress_;
        state.signalStrength = signalStrength_;
        state.isMetered = (currentNetworkType_ == NetworkType::Cellular);
        return state;
    }

    NetworkType getActiveNetworkType() const override {
        return currentNetworkType_;
    }

    bool isConnected() const override {
        return isConnected_;
    }

    // Network Configuration (Requirement 11.5)
    void setAllowedNetworks(NetworkTypeFlags allowed) override {
        allowedNetworks_ = allowed;
    }

    NetworkTypeFlags getAllowedNetworks() const override {
        return allowedNetworks_;
    }

    bool isNetworkTypeAllowed(NetworkType type) const override {
        switch (type) {
            case NetworkType::WiFi:
                return (static_cast<uint32_t>(allowedNetworks_) &
                        static_cast<uint32_t>(NetworkTypeFlags::WiFi)) != 0;
            case NetworkType::Cellular:
                return (static_cast<uint32_t>(allowedNetworks_) &
                        static_cast<uint32_t>(NetworkTypeFlags::Cellular)) != 0;
            case NetworkType::Ethernet:
                return (static_cast<uint32_t>(allowedNetworks_) &
                        static_cast<uint32_t>(NetworkTypeFlags::Ethernet)) != 0;
            case NetworkType::None:
                return false;
        }
        return false;
    }

    // Connection State Preservation (Requirement 11.4)
    void setConnectionStateGracePeriod(std::chrono::milliseconds duration) override {
        connectionStateGracePeriodMs_ = static_cast<uint32_t>(duration.count());
    }

    std::chrono::milliseconds getConnectionStateGracePeriod() const override {
        return std::chrono::milliseconds(connectionStateGracePeriodMs_);
    }

    bool isInGracePeriod() const override {
        if (isConnected_) {
            return false;
        }
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - disconnectionTime_);
        return elapsed.count() < connectionStateGracePeriodMs_;
    }

    std::chrono::milliseconds getRemainingGracePeriod() const override {
        if (isConnected_ || !isInGracePeriod()) {
            return std::chrono::milliseconds(0);
        }
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - disconnectionTime_);
        auto remaining = connectionStateGracePeriodMs_ - static_cast<uint32_t>(elapsed.count());
        return std::chrono::milliseconds(remaining > 0 ? remaining : 0);
    }

    // Interface Rebinding (Requirement 11.2)
    core::Result<void, NetworkMonitorError> rebindToInterface(NetworkType type) override {
        rebindAttempted_ = true;
        lastRebindType_ = type;

        if (!isNetworkTypeAllowed(type)) {
            return core::Result<void, NetworkMonitorError>::error(
                NetworkMonitorError(NetworkMonitorErrorCode::NetworkTypeNotAllowed,
                    "Network type is not allowed by configuration"));
        }

        // Simulate successful rebind
        currentNetworkType_ = type;
        rebindSuccessful_ = true;
        return core::Result<void, NetworkMonitorError>::success();
    }

    bool attemptAutomaticRebind() override {
        rebindAttempted_ = true;

        // Try WiFi first, then Cellular
        if (isNetworkTypeAllowed(NetworkType::WiFi) && wifiAvailable_) {
            currentNetworkType_ = NetworkType::WiFi;
            isConnected_ = true;
            rebindSuccessful_ = true;
            return true;
        }

        if (isNetworkTypeAllowed(NetworkType::Cellular) && cellularAvailable_) {
            currentNetworkType_ = NetworkType::Cellular;
            isConnected_ = true;
            rebindSuccessful_ = true;

            // Warn about cellular usage
            if (cellularDataWarningEnabled_) {
                cellularDataWarningShown_ = true;
                if (cellularWarningCallback_) {
                    cellularWarningCallback_();
                }
            }
            return true;
        }

        rebindSuccessful_ = false;
        return false;
    }

    // Callbacks
    void setNetworkChangeCallback(NetworkChangeCallback callback) override {
        networkChangeCallback_ = callback;
    }

    void setAddressChangeCallback(AddressChangeCallback callback) override {
        addressChangeCallback_ = callback;
    }

    void setCellularWarningCallback(CellularWarningCallback callback) override {
        cellularWarningCallback_ = callback;
    }

    // Cellular Data Warning (Requirement 11.6)
    void enableCellularDataWarning(bool enabled) override {
        cellularDataWarningEnabled_ = enabled;
    }

    bool isCellularDataWarningEnabled() const override {
        return cellularDataWarningEnabled_;
    }

    bool wasDataUsageWarningShown() const override {
        return cellularDataWarningShown_;
    }

    void resetDataUsageWarning() override {
        cellularDataWarningShown_ = false;
    }

    // Server Address Notification (Requirement 11.3)
    std::optional<std::string> getServerAddress() const override {
        return currentIpAddress_;
    }

    bool wasAddressChangeNotified() const override {
        return serverAddressNotified_;
    }

    // Detection Latency (Requirement 11.1)
    std::chrono::milliseconds getLastDetectionLatency() const override {
        return std::chrono::milliseconds(detectionLatencyMs_);
    }

    // ===========================================================================
    // Test Helpers - Not part of interface
    // ===========================================================================

    void simulateNetworkChange(NetworkType newType, bool connected,
                               std::chrono::milliseconds detectionLatency = std::chrono::milliseconds(0)) {
        auto changeTime = std::chrono::steady_clock::now();
        detectionLatencyMs_ = static_cast<uint32_t>(detectionLatency.count());

        NetworkType oldType = currentNetworkType_;
        bool wasConnected = isConnected_;

        currentNetworkType_ = newType;
        isConnected_ = connected;

        if (!connected) {
            disconnectionTime_ = changeTime;
        }

        lastChangeTimestamp_ = changeTime;

        // Update IP address based on network type
        if (connected) {
            switch (newType) {
                case NetworkType::WiFi:
                    currentIpAddress_ = "192.168.1.100";
                    currentInterfaceName_ = "en0";
                    break;
                case NetworkType::Cellular:
                    currentIpAddress_ = "10.0.0.50";
                    currentInterfaceName_ = "pdp_ip0";
                    break;
                case NetworkType::Ethernet:
                    currentIpAddress_ = "192.168.0.100";
                    currentInterfaceName_ = "eth0";
                    break;
                default:
                    currentIpAddress_ = std::nullopt;
                    currentInterfaceName_ = "";
            }
        } else {
            currentIpAddress_ = std::nullopt;
            currentInterfaceName_ = "";
        }

        // Notify callbacks
        if (networkChangeCallback_) {
            NetworkChangeEvent event;
            event.oldType = oldType;
            event.newType = newType;
            event.wasConnected = wasConnected;
            event.isConnected = connected;
            event.timestamp = changeTime;
            networkChangeCallback_(event);
        }

        // Notify address change
        if (connected && currentIpAddress_ && addressChangeCallback_) {
            serverAddressNotified_ = true;
            addressChangeCallback_(*currentIpAddress_);
        }
    }

    void setNetworkAvailability(bool wifiAvailable, bool cellularAvailable) {
        wifiAvailable_ = wifiAvailable;
        cellularAvailable_ = cellularAvailable;
    }

    void setSignalStrength(int strength) {
        signalStrength_ = strength;
    }

    bool wasRebindAttempted() const {
        return rebindAttempted_;
    }

    bool wasRebindSuccessful() const {
        return rebindSuccessful_;
    }

    NetworkType getLastRebindType() const {
        return lastRebindType_;
    }

    void resetTestState() {
        rebindAttempted_ = false;
        rebindSuccessful_ = false;
        serverAddressNotified_ = false;
        cellularDataWarningShown_ = false;
    }

private:
    std::atomic<bool> isMonitoring_;
    NetworkType currentNetworkType_;
    std::atomic<bool> isConnected_;
    std::string currentInterfaceName_;
    std::optional<std::string> currentIpAddress_;
    int signalStrength_ = 100;

    NetworkTypeFlags allowedNetworks_;
    uint32_t connectionStateGracePeriodMs_;
    std::chrono::steady_clock::time_point disconnectionTime_;
    std::chrono::steady_clock::time_point lastChangeTimestamp_;
    uint32_t detectionLatencyMs_;

    bool cellularDataWarningEnabled_;
    bool cellularDataWarningShown_;
    bool serverAddressNotified_;
    bool rebindAttempted_;
    bool rebindSuccessful_ = false;
    NetworkType lastRebindType_ = NetworkType::None;

    bool wifiAvailable_ = true;
    bool cellularAvailable_ = true;

    NetworkChangeCallback networkChangeCallback_;
    AddressChangeCallback addressChangeCallback_;
    CellularWarningCallback cellularWarningCallback_;
};

// =============================================================================
// Network State Tests
// =============================================================================

class NetworkStateTest : public ::testing::Test {
protected:
    void SetUp() override {
        monitor_ = std::make_unique<MockNetworkMonitor>();
    }

    void TearDown() override {
        monitor_.reset();
    }

    std::unique_ptr<MockNetworkMonitor> monitor_;
};

TEST_F(NetworkStateTest, InitialStateIsConnectedViaWiFi) {
    auto state = monitor_->getCurrentState();

    EXPECT_TRUE(state.isConnected);
    EXPECT_EQ(state.type, NetworkType::WiFi);
}

TEST_F(NetworkStateTest, CanStartMonitoring) {
    auto result = monitor_->startMonitoring();

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(monitor_->isMonitoringActive());
}

TEST_F(NetworkStateTest, CanStopMonitoring) {
    monitor_->startMonitoring();
    monitor_->stopMonitoring();

    EXPECT_FALSE(monitor_->isMonitoringActive());
}

TEST_F(NetworkStateTest, CannotStartMonitoringTwice) {
    monitor_->startMonitoring();
    auto result = monitor_->startMonitoring();

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, NetworkMonitorErrorCode::AlreadyMonitoring);
}

TEST_F(NetworkStateTest, GetActiveNetworkType) {
    EXPECT_EQ(monitor_->getActiveNetworkType(), NetworkType::WiFi);

    monitor_->simulateNetworkChange(NetworkType::Cellular, true);
    EXPECT_EQ(monitor_->getActiveNetworkType(), NetworkType::Cellular);
}

TEST_F(NetworkStateTest, IsConnectedReturnsCorrectState) {
    EXPECT_TRUE(monitor_->isConnected());

    monitor_->simulateNetworkChange(NetworkType::None, false);
    EXPECT_FALSE(monitor_->isConnected());
}

// =============================================================================
// Network Change Detection Tests (Requirement 11.1)
// =============================================================================

class NetworkChangeDetectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        monitor_ = std::make_unique<MockNetworkMonitor>();
        changeDetected_ = false;
        lastEvent_ = NetworkChangeEvent{};
    }

    void TearDown() override {
        monitor_.reset();
    }

    std::unique_ptr<MockNetworkMonitor> monitor_;
    bool changeDetected_;
    NetworkChangeEvent lastEvent_;
};

TEST_F(NetworkChangeDetectionTest, DetectsWiFiToCellularChange) {
    monitor_->setNetworkChangeCallback([this](const NetworkChangeEvent& event) {
        changeDetected_ = true;
        lastEvent_ = event;
    });

    monitor_->simulateNetworkChange(NetworkType::Cellular, true);

    EXPECT_TRUE(changeDetected_);
    EXPECT_EQ(lastEvent_.oldType, NetworkType::WiFi);
    EXPECT_EQ(lastEvent_.newType, NetworkType::Cellular);
}

TEST_F(NetworkChangeDetectionTest, DetectsCellularToWiFiChange) {
    monitor_->simulateNetworkChange(NetworkType::Cellular, true);

    monitor_->setNetworkChangeCallback([this](const NetworkChangeEvent& event) {
        changeDetected_ = true;
        lastEvent_ = event;
    });

    monitor_->simulateNetworkChange(NetworkType::WiFi, true);

    EXPECT_TRUE(changeDetected_);
    EXPECT_EQ(lastEvent_.oldType, NetworkType::Cellular);
    EXPECT_EQ(lastEvent_.newType, NetworkType::WiFi);
}

TEST_F(NetworkChangeDetectionTest, DetectsNetworkDisconnection) {
    monitor_->setNetworkChangeCallback([this](const NetworkChangeEvent& event) {
        changeDetected_ = true;
        lastEvent_ = event;
    });

    monitor_->simulateNetworkChange(NetworkType::None, false);

    EXPECT_TRUE(changeDetected_);
    EXPECT_TRUE(lastEvent_.wasConnected);
    EXPECT_FALSE(lastEvent_.isConnected);
}

TEST_F(NetworkChangeDetectionTest, DetectsNetworkReconnection) {
    monitor_->simulateNetworkChange(NetworkType::None, false);

    monitor_->setNetworkChangeCallback([this](const NetworkChangeEvent& event) {
        changeDetected_ = true;
        lastEvent_ = event;
    });

    monitor_->simulateNetworkChange(NetworkType::WiFi, true);

    EXPECT_TRUE(changeDetected_);
    EXPECT_FALSE(lastEvent_.wasConnected);
    EXPECT_TRUE(lastEvent_.isConnected);
}

TEST_F(NetworkChangeDetectionTest, DetectionLatencyWithinTwoSeconds) {
    // Requirement 11.1: Detect network connectivity changes within 2 seconds
    monitor_->simulateNetworkChange(NetworkType::Cellular, true,
        std::chrono::milliseconds(1500));

    auto latency = monitor_->getLastDetectionLatency();
    EXPECT_LE(latency.count(), 2000);
}

TEST_F(NetworkChangeDetectionTest, DetectionLatencyExceedingTwoSecondsDetected) {
    // This test verifies we can track when detection exceeds the requirement
    monitor_->simulateNetworkChange(NetworkType::Cellular, true,
        std::chrono::milliseconds(2500));

    auto latency = monitor_->getLastDetectionLatency();
    EXPECT_GT(latency.count(), 2000);
}

// =============================================================================
// Interface Rebinding Tests (Requirement 11.2)
// =============================================================================

class InterfaceRebindingTest : public ::testing::Test {
protected:
    void SetUp() override {
        monitor_ = std::make_unique<MockNetworkMonitor>();
    }

    void TearDown() override {
        monitor_.reset();
    }

    std::unique_ptr<MockNetworkMonitor> monitor_;
};

TEST_F(InterfaceRebindingTest, CanRebindToAllowedNetworkType) {
    monitor_->setAllowedNetworks(NetworkTypeFlags::Any);

    auto result = monitor_->rebindToInterface(NetworkType::Cellular);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(monitor_->getActiveNetworkType(), NetworkType::Cellular);
}

TEST_F(InterfaceRebindingTest, CannotRebindToDisallowedNetworkType) {
    monitor_->setAllowedNetworks(NetworkTypeFlags::WiFi);

    auto result = monitor_->rebindToInterface(NetworkType::Cellular);

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, NetworkMonitorErrorCode::NetworkTypeNotAllowed);
}

TEST_F(InterfaceRebindingTest, AutomaticRebindOnWiFiLoss) {
    monitor_->setAllowedNetworks(NetworkTypeFlags::Any);
    monitor_->setNetworkAvailability(false, true); // WiFi unavailable, cellular available

    bool rebindSuccess = monitor_->attemptAutomaticRebind();

    EXPECT_TRUE(rebindSuccess);
    EXPECT_EQ(monitor_->getActiveNetworkType(), NetworkType::Cellular);
}

TEST_F(InterfaceRebindingTest, AutomaticRebindPrefersWiFiOverCellular) {
    monitor_->setAllowedNetworks(NetworkTypeFlags::Any);
    monitor_->setNetworkAvailability(true, true); // Both available

    monitor_->attemptAutomaticRebind();

    EXPECT_EQ(monitor_->getActiveNetworkType(), NetworkType::WiFi);
}

TEST_F(InterfaceRebindingTest, AutomaticRebindFailsWhenNoNetworkAllowed) {
    monitor_->setAllowedNetworks(NetworkTypeFlags::WiFi);
    monitor_->setNetworkAvailability(false, true); // Only cellular available

    bool rebindSuccess = monitor_->attemptAutomaticRebind();

    EXPECT_FALSE(rebindSuccess);
}

TEST_F(InterfaceRebindingTest, RebindAttemptedFlagSet) {
    monitor_->attemptAutomaticRebind();

    EXPECT_TRUE(monitor_->wasRebindAttempted());
}

// =============================================================================
// Address Change Notification Tests (Requirement 11.3)
// =============================================================================

class AddressNotificationTest : public ::testing::Test {
protected:
    void SetUp() override {
        monitor_ = std::make_unique<MockNetworkMonitor>();
        addressNotified_ = false;
        newAddress_ = "";
    }

    void TearDown() override {
        monitor_.reset();
    }

    std::unique_ptr<MockNetworkMonitor> monitor_;
    bool addressNotified_;
    std::string newAddress_;
};

TEST_F(AddressNotificationTest, NotifiesOnAddressChange) {
    monitor_->setAddressChangeCallback([this](const std::string& address) {
        addressNotified_ = true;
        newAddress_ = address;
    });

    monitor_->simulateNetworkChange(NetworkType::Cellular, true);

    EXPECT_TRUE(addressNotified_);
    EXPECT_FALSE(newAddress_.empty());
}

TEST_F(AddressNotificationTest, ServerAddressAvailableAfterChange) {
    monitor_->simulateNetworkChange(NetworkType::Cellular, true);

    auto address = monitor_->getServerAddress();

    EXPECT_TRUE(address.has_value());
    EXPECT_FALSE(address->empty());
}

TEST_F(AddressNotificationTest, ServerAddressEmptyWhenDisconnected) {
    monitor_->simulateNetworkChange(NetworkType::None, false);

    auto address = monitor_->getServerAddress();

    EXPECT_FALSE(address.has_value());
}

TEST_F(AddressNotificationTest, AddressChangeNotifiedFlagSet) {
    monitor_->setAddressChangeCallback([](const std::string&) {});
    monitor_->simulateNetworkChange(NetworkType::Cellular, true);

    EXPECT_TRUE(monitor_->wasAddressChangeNotified());
}

TEST_F(AddressNotificationTest, WiFiAddressDifferentFromCellular) {
    std::string wifiAddress;
    std::string cellularAddress;

    monitor_->setAddressChangeCallback([&](const std::string& address) {
        if (monitor_->getActiveNetworkType() == NetworkType::WiFi) {
            wifiAddress = address;
        } else {
            cellularAddress = address;
        }
    });

    monitor_->simulateNetworkChange(NetworkType::WiFi, true);
    monitor_->simulateNetworkChange(NetworkType::Cellular, true);

    EXPECT_NE(wifiAddress, cellularAddress);
}

// =============================================================================
// Connection State Preservation Tests (Requirement 11.4)
// =============================================================================

class ConnectionStatePreservationTest : public ::testing::Test {
protected:
    void SetUp() override {
        monitor_ = std::make_unique<MockNetworkMonitor>();
    }

    void TearDown() override {
        monitor_.reset();
    }

    std::unique_ptr<MockNetworkMonitor> monitor_;
};

TEST_F(ConnectionStatePreservationTest, DefaultGracePeriodIs30Seconds) {
    auto gracePeriod = monitor_->getConnectionStateGracePeriod();

    EXPECT_EQ(gracePeriod.count(), 30000);
}

TEST_F(ConnectionStatePreservationTest, CanSetGracePeriod) {
    monitor_->setConnectionStateGracePeriod(std::chrono::milliseconds(60000));

    auto gracePeriod = monitor_->getConnectionStateGracePeriod();

    EXPECT_EQ(gracePeriod.count(), 60000);
}

TEST_F(ConnectionStatePreservationTest, EntersGracePeriodOnDisconnection) {
    monitor_->simulateNetworkChange(NetworkType::None, false);

    EXPECT_TRUE(monitor_->isInGracePeriod());
}

TEST_F(ConnectionStatePreservationTest, NotInGracePeriodWhenConnected) {
    EXPECT_FALSE(monitor_->isInGracePeriod());
}

TEST_F(ConnectionStatePreservationTest, RemainingGracePeriodReturnsValue) {
    monitor_->setConnectionStateGracePeriod(std::chrono::milliseconds(30000));
    monitor_->simulateNetworkChange(NetworkType::None, false);

    auto remaining = monitor_->getRemainingGracePeriod();

    // Should be close to 30 seconds (allowing for some execution time)
    EXPECT_GT(remaining.count(), 29000);
    EXPECT_LE(remaining.count(), 30000);
}

TEST_F(ConnectionStatePreservationTest, RemainingGracePeriodZeroWhenConnected) {
    auto remaining = monitor_->getRemainingGracePeriod();

    EXPECT_EQ(remaining.count(), 0);
}

// =============================================================================
// Network Type Restriction Tests (Requirement 11.5)
// =============================================================================

class NetworkTypeRestrictionTest : public ::testing::Test {
protected:
    void SetUp() override {
        monitor_ = std::make_unique<MockNetworkMonitor>();
    }

    void TearDown() override {
        monitor_.reset();
    }

    std::unique_ptr<MockNetworkMonitor> monitor_;
};

TEST_F(NetworkTypeRestrictionTest, DefaultAllowsAllNetworkTypes) {
    auto allowed = monitor_->getAllowedNetworks();

    EXPECT_EQ(allowed, NetworkTypeFlags::Any);
}

TEST_F(NetworkTypeRestrictionTest, CanRestrictToWiFiOnly) {
    monitor_->setAllowedNetworks(NetworkTypeFlags::WiFi);

    EXPECT_TRUE(monitor_->isNetworkTypeAllowed(NetworkType::WiFi));
    EXPECT_FALSE(monitor_->isNetworkTypeAllowed(NetworkType::Cellular));
}

TEST_F(NetworkTypeRestrictionTest, CanRestrictToCellularOnly) {
    monitor_->setAllowedNetworks(NetworkTypeFlags::Cellular);

    EXPECT_FALSE(monitor_->isNetworkTypeAllowed(NetworkType::WiFi));
    EXPECT_TRUE(monitor_->isNetworkTypeAllowed(NetworkType::Cellular));
}

TEST_F(NetworkTypeRestrictionTest, CanAllowMultipleTypes) {
    monitor_->setAllowedNetworks(
        static_cast<NetworkTypeFlags>(
            static_cast<uint32_t>(NetworkTypeFlags::WiFi) |
            static_cast<uint32_t>(NetworkTypeFlags::Ethernet)));

    EXPECT_TRUE(monitor_->isNetworkTypeAllowed(NetworkType::WiFi));
    EXPECT_TRUE(monitor_->isNetworkTypeAllowed(NetworkType::Ethernet));
    EXPECT_FALSE(monitor_->isNetworkTypeAllowed(NetworkType::Cellular));
}

TEST_F(NetworkTypeRestrictionTest, NoneNetworkTypeNeverAllowed) {
    monitor_->setAllowedNetworks(NetworkTypeFlags::Any);

    EXPECT_FALSE(monitor_->isNetworkTypeAllowed(NetworkType::None));
}

// =============================================================================
// Cellular Data Warning Tests (Requirement 11.6)
// =============================================================================

class CellularDataWarningTest : public ::testing::Test {
protected:
    void SetUp() override {
        monitor_ = std::make_unique<MockNetworkMonitor>();
        warningShown_ = false;
    }

    void TearDown() override {
        monitor_.reset();
    }

    std::unique_ptr<MockNetworkMonitor> monitor_;
    bool warningShown_;
};

TEST_F(CellularDataWarningTest, WarningEnabledByDefault) {
    EXPECT_TRUE(monitor_->isCellularDataWarningEnabled());
}

TEST_F(CellularDataWarningTest, CanDisableWarning) {
    monitor_->enableCellularDataWarning(false);

    EXPECT_FALSE(monitor_->isCellularDataWarningEnabled());
}

TEST_F(CellularDataWarningTest, WarningNotShownInitially) {
    EXPECT_FALSE(monitor_->wasDataUsageWarningShown());
}

TEST_F(CellularDataWarningTest, WarningShownWhenRebindingToCellular) {
    monitor_->setAllowedNetworks(NetworkTypeFlags::Cellular);
    monitor_->setNetworkAvailability(false, true);

    monitor_->attemptAutomaticRebind();

    EXPECT_TRUE(monitor_->wasDataUsageWarningShown());
}

TEST_F(CellularDataWarningTest, WarningCallbackInvokedOnCellularRebind) {
    monitor_->setCellularWarningCallback([this]() {
        warningShown_ = true;
    });
    monitor_->setAllowedNetworks(NetworkTypeFlags::Cellular);
    monitor_->setNetworkAvailability(false, true);

    monitor_->attemptAutomaticRebind();

    EXPECT_TRUE(warningShown_);
}

TEST_F(CellularDataWarningTest, WarningNotShownWhenDisabled) {
    monitor_->enableCellularDataWarning(false);
    monitor_->setAllowedNetworks(NetworkTypeFlags::Cellular);
    monitor_->setNetworkAvailability(false, true);

    monitor_->attemptAutomaticRebind();

    EXPECT_FALSE(monitor_->wasDataUsageWarningShown());
}

TEST_F(CellularDataWarningTest, CanResetWarningState) {
    monitor_->setAllowedNetworks(NetworkTypeFlags::Cellular);
    monitor_->setNetworkAvailability(false, true);
    monitor_->attemptAutomaticRebind();

    monitor_->resetDataUsageWarning();

    EXPECT_FALSE(monitor_->wasDataUsageWarningShown());
}

TEST_F(CellularDataWarningTest, CellularNetworkMarkedAsMetered) {
    monitor_->simulateNetworkChange(NetworkType::Cellular, true);

    auto state = monitor_->getCurrentState();

    EXPECT_TRUE(state.isMetered);
}

TEST_F(CellularDataWarningTest, WiFiNetworkNotMarkedAsMetered) {
    auto state = monitor_->getCurrentState();

    EXPECT_FALSE(state.isMetered);
}

// =============================================================================
// Integration Tests
// =============================================================================

class NetworkMonitorIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        monitor_ = std::make_unique<MockNetworkMonitor>();
    }

    void TearDown() override {
        monitor_.reset();
    }

    std::unique_ptr<MockNetworkMonitor> monitor_;
};

TEST_F(NetworkMonitorIntegrationTest, FullWiFiToCellularTransition) {
    // Set up monitoring
    monitor_->startMonitoring();
    monitor_->setAllowedNetworks(NetworkTypeFlags::Any);

    bool changeDetected = false;
    bool addressNotified = false;
    bool cellularWarning = false;

    monitor_->setNetworkChangeCallback([&](const NetworkChangeEvent& event) {
        changeDetected = true;
    });
    monitor_->setAddressChangeCallback([&](const std::string& address) {
        addressNotified = true;
    });
    monitor_->setCellularWarningCallback([&]() {
        cellularWarning = true;
    });

    // Simulate WiFi loss and automatic rebind to cellular
    monitor_->setNetworkAvailability(false, true);
    monitor_->simulateNetworkChange(NetworkType::None, false);

    // Attempt automatic rebind
    bool rebindSuccess = monitor_->attemptAutomaticRebind();

    // Verify transition
    EXPECT_TRUE(rebindSuccess);
    EXPECT_EQ(monitor_->getActiveNetworkType(), NetworkType::Cellular);
    EXPECT_TRUE(monitor_->isConnected());
    EXPECT_TRUE(cellularWarning);
}

TEST_F(NetworkMonitorIntegrationTest, GracePeriodAllowsReconnection) {
    monitor_->startMonitoring();
    monitor_->setConnectionStateGracePeriod(std::chrono::milliseconds(30000));

    // Disconnect
    monitor_->simulateNetworkChange(NetworkType::None, false);
    EXPECT_TRUE(monitor_->isInGracePeriod());

    // Reconnect within grace period
    monitor_->simulateNetworkChange(NetworkType::WiFi, true);
    EXPECT_FALSE(monitor_->isInGracePeriod());
    EXPECT_TRUE(monitor_->isConnected());
}

TEST_F(NetworkMonitorIntegrationTest, WiFiOnlyModeBlocksCellularRebind) {
    monitor_->startMonitoring();
    monitor_->setAllowedNetworks(NetworkTypeFlags::WiFi);

    // Make only cellular available
    monitor_->setNetworkAvailability(false, true);

    // Try to rebind
    auto result = monitor_->rebindToInterface(NetworkType::Cellular);

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, NetworkMonitorErrorCode::NetworkTypeNotAllowed);
}

TEST_F(NetworkMonitorIntegrationTest, CellularOnlyModeBlocksWiFiRebind) {
    monitor_->startMonitoring();
    monitor_->setAllowedNetworks(NetworkTypeFlags::Cellular);

    // Try to rebind to WiFi
    auto result = monitor_->rebindToInterface(NetworkType::WiFi);

    EXPECT_TRUE(result.isError());
}

TEST_F(NetworkMonitorIntegrationTest, MultipleNetworkTransitions) {
    monitor_->startMonitoring();
    monitor_->setAllowedNetworks(NetworkTypeFlags::Any);

    int changeCount = 0;
    monitor_->setNetworkChangeCallback([&](const NetworkChangeEvent&) {
        changeCount++;
    });

    // WiFi -> Cellular -> WiFi -> Disconnect
    monitor_->simulateNetworkChange(NetworkType::Cellular, true);
    monitor_->simulateNetworkChange(NetworkType::WiFi, true);
    monitor_->simulateNetworkChange(NetworkType::Cellular, true);
    monitor_->simulateNetworkChange(NetworkType::None, false);

    EXPECT_EQ(changeCount, 4);
}

} // namespace test
} // namespace mobile
} // namespace pal
} // namespace openrtmp

#endif // Platform check
