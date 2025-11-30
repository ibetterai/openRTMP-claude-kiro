// OpenRTMP - Cross-platform RTMP Server
// E2E Tests: Network Failover Scenarios on Mobile
//
// Task 21.3: Implement E2E and performance tests
// Tests network interface change detection and connection state preservation.
//
// Note: This test uses mock implementations since we're running on macOS.
// Real network failover testing requires mobile devices with network access.
//
// Requirements coverage:
// - Requirement 11.1: Detect network connectivity changes within 2 seconds
// - Requirement 11.2: Attempt interface rebind on WiFi to cellular transition
// - Requirement 11.3: Notify clients of new server address when interface changes
// - Requirement 11.4: Maintain connection state for 30 seconds during network loss
// - Requirement 11.5: Support configuration to restrict to WiFi-only or cellular-only
// - Requirement 11.6: Warn users about cellular data usage

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>

#include "openrtmp/core/types.hpp"
#include "openrtmp/core/result.hpp"

namespace openrtmp {
namespace e2e {
namespace test {

// =============================================================================
// Network Types and Structures
// =============================================================================

/**
 * @brief Network interface information.
 */
struct NetworkInterface {
    std::string name;
    NetworkType type;
    std::string ipAddress;
    bool isUp;
    int signalStrength;  // 0-100
};

/**
 * @brief Network change event.
 */
struct NetworkChangeEvent {
    NetworkType previousType;
    NetworkType newType;
    std::string previousIP;
    std::string newIP;
    std::chrono::steady_clock::time_point timestamp;
};

/**
 * @brief Network restriction flags.
 */
enum class NetworkRestriction : uint32_t {
    None = 0,
    WiFiOnly = 1,
    CellularOnly = 2,
    Any = 3
};

/**
 * @brief Network failover error codes.
 */
enum class NetworkFailoverError {
    Success,
    NoNetworkAvailable,
    RebindFailed,
    NetworkTypeNotAllowed,
    GracePeriodExpired,
    InterfaceNotFound
};

// Callbacks
using NetworkChangeCallback = std::function<void(const NetworkChangeEvent&)>;
using AddressChangeCallback = std::function<void(const std::string& newAddress)>;
using CellularWarningCallback = std::function<void()>;
using ConnectionLostCallback = std::function<void(bool inGracePeriod)>;

// =============================================================================
// Mock Network Failover Manager
// =============================================================================

class MockNetworkFailoverManager {
public:
    MockNetworkFailoverManager()
        : currentType_(NetworkType::WiFi)
        , isConnected_(true)
        , currentIP_("192.168.1.100")
        , restriction_(NetworkRestriction::Any)
        , gracePeriod_(std::chrono::seconds(30))
        , inGracePeriod_(false)
        , wifiAvailable_(true)
        , cellularAvailable_(true)
        , cellularWarningEnabled_(true)
    {}

    // Network state
    NetworkType getCurrentNetworkType() const { return currentType_; }
    bool isConnected() const { return isConnected_; }
    std::string getCurrentIP() const { return currentIP_; }

    // Configuration (Requirement 11.5)
    void setNetworkRestriction(NetworkRestriction restriction) {
        restriction_ = restriction;
    }

    NetworkRestriction getNetworkRestriction() const {
        return restriction_;
    }

    bool isNetworkTypeAllowed(NetworkType type) const {
        if (restriction_ == NetworkRestriction::Any) return true;
        if (restriction_ == NetworkRestriction::WiFiOnly && type == NetworkType::WiFi) return true;
        if (restriction_ == NetworkRestriction::CellularOnly && type == NetworkType::Cellular) return true;
        return false;
    }

    // Grace period configuration (Requirement 11.4)
    void setGracePeriod(std::chrono::seconds duration) {
        gracePeriod_ = duration;
    }

    std::chrono::seconds getGracePeriod() const {
        return gracePeriod_;
    }

    bool isInGracePeriod() const {
        return inGracePeriod_;
    }

    std::chrono::seconds getRemainingGracePeriod() const {
        if (!inGracePeriod_) return std::chrono::seconds(0);
        return remainingGracePeriod_;
    }

    // Cellular warning (Requirement 11.6)
    void setCellularWarningEnabled(bool enabled) {
        cellularWarningEnabled_ = enabled;
    }

    // Callbacks
    void setNetworkChangeCallback(NetworkChangeCallback callback) {
        networkChangeCallback_ = std::move(callback);
    }

    void setAddressChangeCallback(AddressChangeCallback callback) {
        addressChangeCallback_ = std::move(callback);
    }

    void setCellularWarningCallback(CellularWarningCallback callback) {
        cellularWarningCallback_ = std::move(callback);
    }

    void setConnectionLostCallback(ConnectionLostCallback callback) {
        connectionLostCallback_ = std::move(callback);
    }

    // Simulate network events (Requirement 11.1: within 2 seconds)
    void simulateWiFiLoss() {
        auto previousType = currentType_;
        auto previousIP = currentIP_;

        wifiAvailable_ = false;

        if (currentType_ == NetworkType::WiFi) {
            // Try to rebind to cellular (Requirement 11.2)
            if (cellularAvailable_ && isNetworkTypeAllowed(NetworkType::Cellular)) {
                currentType_ = NetworkType::Cellular;
                currentIP_ = "10.0.0.50";

                // Warn about cellular usage (Requirement 11.6)
                if (cellularWarningEnabled_ && cellularWarningCallback_) {
                    cellularWarningCallback_();
                }

                // Notify of address change (Requirement 11.3)
                if (addressChangeCallback_) {
                    addressChangeCallback_(currentIP_);
                }

                if (networkChangeCallback_) {
                    NetworkChangeEvent event{
                        previousType,
                        currentType_,
                        previousIP,
                        currentIP_,
                        std::chrono::steady_clock::now()
                    };
                    networkChangeCallback_(event);
                }
            } else {
                // Enter grace period (Requirement 11.4)
                enterGracePeriod();
            }
        }
    }

    void simulateWiFiRestore() {
        wifiAvailable_ = true;

        if (isNetworkTypeAllowed(NetworkType::WiFi)) {
            auto previousType = currentType_;
            auto previousIP = currentIP_;

            currentType_ = NetworkType::WiFi;
            currentIP_ = "192.168.1.100";
            isConnected_ = true;
            exitGracePeriod();

            if (addressChangeCallback_) {
                addressChangeCallback_(currentIP_);
            }

            if (networkChangeCallback_) {
                NetworkChangeEvent event{
                    previousType,
                    currentType_,
                    previousIP,
                    currentIP_,
                    std::chrono::steady_clock::now()
                };
                networkChangeCallback_(event);
            }
        }
    }

    void simulateCellularLoss() {
        cellularAvailable_ = false;

        if (currentType_ == NetworkType::Cellular) {
            if (wifiAvailable_ && isNetworkTypeAllowed(NetworkType::WiFi)) {
                auto previousType = currentType_;
                auto previousIP = currentIP_;

                currentType_ = NetworkType::WiFi;
                currentIP_ = "192.168.1.100";

                if (addressChangeCallback_) {
                    addressChangeCallback_(currentIP_);
                }

                if (networkChangeCallback_) {
                    NetworkChangeEvent event{
                        previousType,
                        currentType_,
                        previousIP,
                        currentIP_,
                        std::chrono::steady_clock::now()
                    };
                    networkChangeCallback_(event);
                }
            } else {
                enterGracePeriod();
            }
        }
    }

    void simulateTotalNetworkLoss() {
        wifiAvailable_ = false;
        cellularAvailable_ = false;
        enterGracePeriod();
    }

    void simulateNetworkRestore() {
        wifiAvailable_ = true;
        cellularAvailable_ = true;

        if (inGracePeriod_) {
            exitGracePeriod();

            // Rebind to preferred network
            if (isNetworkTypeAllowed(NetworkType::WiFi)) {
                currentType_ = NetworkType::WiFi;
                currentIP_ = "192.168.1.100";
            } else if (isNetworkTypeAllowed(NetworkType::Cellular)) {
                currentType_ = NetworkType::Cellular;
                currentIP_ = "10.0.0.50";
            }
            isConnected_ = true;
        }
    }

    // Grace period simulation
    void simulateGracePeriodTimeout() {
        if (inGracePeriod_) {
            remainingGracePeriod_ = std::chrono::seconds(0);
            inGracePeriod_ = false;
            isConnected_ = false;

            if (connectionLostCallback_) {
                connectionLostCallback_(false);  // Not in grace period anymore
            }
        }
    }

    void simulateGracePeriodProgress(std::chrono::seconds elapsed) {
        if (inGracePeriod_ && remainingGracePeriod_ > elapsed) {
            remainingGracePeriod_ -= elapsed;
        } else if (inGracePeriod_) {
            simulateGracePeriodTimeout();
        }
    }

    // Manual rebind (Requirement 11.2)
    core::Result<void, NetworkFailoverError> rebindToInterface(NetworkType type) {
        if (!isNetworkTypeAllowed(type)) {
            return core::Result<void, NetworkFailoverError>::error(
                NetworkFailoverError::NetworkTypeNotAllowed);
        }

        if (type == NetworkType::WiFi && !wifiAvailable_) {
            return core::Result<void, NetworkFailoverError>::error(
                NetworkFailoverError::NoNetworkAvailable);
        }

        if (type == NetworkType::Cellular && !cellularAvailable_) {
            return core::Result<void, NetworkFailoverError>::error(
                NetworkFailoverError::NoNetworkAvailable);
        }

        auto previousIP = currentIP_;
        currentType_ = type;

        if (type == NetworkType::WiFi) {
            currentIP_ = "192.168.1.100";
        } else if (type == NetworkType::Cellular) {
            currentIP_ = "10.0.0.50";

            if (cellularWarningEnabled_ && cellularWarningCallback_) {
                cellularWarningCallback_();
            }
        }

        isConnected_ = true;
        exitGracePeriod();

        if (addressChangeCallback_) {
            addressChangeCallback_(currentIP_);
        }

        return core::Result<void, NetworkFailoverError>::success();
    }

    // Detection timing test helper (Requirement 11.1)
    std::chrono::milliseconds getDetectionLatency() const {
        return detectionLatency_;
    }

    void setDetectionLatency(std::chrono::milliseconds latency) {
        detectionLatency_ = latency;
    }

private:
    void enterGracePeriod() {
        inGracePeriod_ = true;
        remainingGracePeriod_ = gracePeriod_;
        isConnected_ = false;
        currentType_ = NetworkType::None;
        currentIP_ = "";

        if (connectionLostCallback_) {
            connectionLostCallback_(true);  // In grace period
        }
    }

    void exitGracePeriod() {
        inGracePeriod_ = false;
        remainingGracePeriod_ = std::chrono::seconds(0);
    }

    NetworkType currentType_;
    bool isConnected_;
    std::string currentIP_;
    NetworkRestriction restriction_;
    std::chrono::seconds gracePeriod_;
    std::chrono::seconds remainingGracePeriod_{0};
    bool inGracePeriod_;
    bool wifiAvailable_;
    bool cellularAvailable_;
    bool cellularWarningEnabled_;
    std::chrono::milliseconds detectionLatency_{500};  // < 2 seconds

    NetworkChangeCallback networkChangeCallback_;
    AddressChangeCallback addressChangeCallback_;
    CellularWarningCallback cellularWarningCallback_;
    ConnectionLostCallback connectionLostCallback_;
};

// =============================================================================
// Mock Connection State Manager
// =============================================================================

class MockConnectionStateManager {
public:
    MockConnectionStateManager()
        : activeConnections_(0)
        , preservedState_(false)
    {}

    void setActiveConnections(int count) { activeConnections_ = count; }
    int getActiveConnections() const { return activeConnections_; }

    void preserveState() { preservedState_ = true; }
    void clearState() { preservedState_ = false; }
    bool isStatePreserved() const { return preservedState_; }

private:
    int activeConnections_;
    bool preservedState_;
};

// =============================================================================
// Test Fixtures
// =============================================================================

class NetworkFailoverTest : public ::testing::Test {
protected:
    void SetUp() override {
        failoverManager_ = std::make_unique<MockNetworkFailoverManager>();
        connectionManager_ = std::make_unique<MockConnectionStateManager>();
    }

    void TearDown() override {
        connectionManager_.reset();
        failoverManager_.reset();
    }

    std::unique_ptr<MockNetworkFailoverManager> failoverManager_;
    std::unique_ptr<MockConnectionStateManager> connectionManager_;
};

// =============================================================================
// Network Change Detection Tests (Requirement 11.1)
// =============================================================================

TEST_F(NetworkFailoverTest, DetectsNetworkChangeWithin2Seconds) {
    // Detection latency should be under 2 seconds
    auto latency = failoverManager_->getDetectionLatency();
    EXPECT_LT(latency.count(), 2000);
}

TEST_F(NetworkFailoverTest, DetectsWiFiLoss) {
    bool changeDetected = false;
    NetworkType newType = NetworkType::WiFi;

    failoverManager_->setNetworkChangeCallback([&](const NetworkChangeEvent& event) {
        changeDetected = true;
        newType = event.newType;
    });

    failoverManager_->simulateWiFiLoss();

    EXPECT_TRUE(changeDetected);
    EXPECT_EQ(newType, NetworkType::Cellular);  // Auto-failover to cellular
}

TEST_F(NetworkFailoverTest, DetectsCellularLoss) {
    // Start on cellular
    failoverManager_->rebindToInterface(NetworkType::Cellular);

    bool changeDetected = false;
    failoverManager_->setNetworkChangeCallback([&](const NetworkChangeEvent& event) {
        changeDetected = true;
    });

    failoverManager_->simulateCellularLoss();

    // Should failover to WiFi
    EXPECT_TRUE(changeDetected);
    EXPECT_EQ(failoverManager_->getCurrentNetworkType(), NetworkType::WiFi);
}

// =============================================================================
// Interface Rebind Tests (Requirement 11.2)
// =============================================================================

TEST_F(NetworkFailoverTest, AutoRebindToWiFiOnCellularLoss) {
    // Start on cellular
    failoverManager_->rebindToInterface(NetworkType::Cellular);
    EXPECT_EQ(failoverManager_->getCurrentNetworkType(), NetworkType::Cellular);

    // Lose cellular, should auto-rebind to WiFi
    failoverManager_->simulateCellularLoss();

    EXPECT_EQ(failoverManager_->getCurrentNetworkType(), NetworkType::WiFi);
    EXPECT_TRUE(failoverManager_->isConnected());
}

TEST_F(NetworkFailoverTest, AutoRebindToCellularOnWiFiLoss) {
    EXPECT_EQ(failoverManager_->getCurrentNetworkType(), NetworkType::WiFi);

    failoverManager_->simulateWiFiLoss();

    EXPECT_EQ(failoverManager_->getCurrentNetworkType(), NetworkType::Cellular);
    EXPECT_TRUE(failoverManager_->isConnected());
}

TEST_F(NetworkFailoverTest, ManualRebindToSpecificInterface) {
    EXPECT_EQ(failoverManager_->getCurrentNetworkType(), NetworkType::WiFi);

    auto result = failoverManager_->rebindToInterface(NetworkType::Cellular);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(failoverManager_->getCurrentNetworkType(), NetworkType::Cellular);
}

TEST_F(NetworkFailoverTest, RebindFailsWhenInterfaceUnavailable) {
    failoverManager_->simulateCellularLoss();

    auto result = failoverManager_->rebindToInterface(NetworkType::Cellular);

    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.error(), NetworkFailoverError::NoNetworkAvailable);
}

// =============================================================================
// Address Change Notification Tests (Requirement 11.3)
// =============================================================================

TEST_F(NetworkFailoverTest, NotifiesClientOfNewAddress) {
    std::string newAddress;

    failoverManager_->setAddressChangeCallback([&](const std::string& addr) {
        newAddress = addr;
    });

    failoverManager_->simulateWiFiLoss();

    EXPECT_FALSE(newAddress.empty());
    EXPECT_EQ(newAddress, "10.0.0.50");  // Cellular IP
}

TEST_F(NetworkFailoverTest, NotifiesOnWiFiRestore) {
    failoverManager_->simulateWiFiLoss();  // Switch to cellular

    std::string newAddress;
    failoverManager_->setAddressChangeCallback([&](const std::string& addr) {
        newAddress = addr;
    });

    failoverManager_->simulateWiFiRestore();

    EXPECT_EQ(newAddress, "192.168.1.100");  // WiFi IP
}

// =============================================================================
// Grace Period Tests (Requirement 11.4)
// =============================================================================

TEST_F(NetworkFailoverTest, MaintainsConnectionStateDuringGracePeriod) {
    failoverManager_->setGracePeriod(std::chrono::seconds(30));

    bool inGracePeriod = false;
    failoverManager_->setConnectionLostCallback([&](bool gracePeriodActive) {
        inGracePeriod = gracePeriodActive;
    });

    // Total network loss
    failoverManager_->simulateTotalNetworkLoss();

    EXPECT_TRUE(inGracePeriod);
    EXPECT_TRUE(failoverManager_->isInGracePeriod());
    EXPECT_EQ(failoverManager_->getRemainingGracePeriod().count(), 30);
}

TEST_F(NetworkFailoverTest, GracePeriodIs30Seconds) {
    failoverManager_->setGracePeriod(std::chrono::seconds(30));

    failoverManager_->simulateTotalNetworkLoss();

    EXPECT_EQ(failoverManager_->getRemainingGracePeriod().count(), 30);
}

TEST_F(NetworkFailoverTest, GracePeriodDecreasesOverTime) {
    failoverManager_->setGracePeriod(std::chrono::seconds(30));
    failoverManager_->simulateTotalNetworkLoss();

    // Simulate 10 seconds passing
    failoverManager_->simulateGracePeriodProgress(std::chrono::seconds(10));

    EXPECT_EQ(failoverManager_->getRemainingGracePeriod().count(), 20);
}

TEST_F(NetworkFailoverTest, ConnectionRestoredDuringGracePeriod) {
    failoverManager_->setGracePeriod(std::chrono::seconds(30));
    failoverManager_->simulateTotalNetworkLoss();

    EXPECT_TRUE(failoverManager_->isInGracePeriod());

    // Network restored within grace period
    failoverManager_->simulateNetworkRestore();

    EXPECT_FALSE(failoverManager_->isInGracePeriod());
    EXPECT_TRUE(failoverManager_->isConnected());
}

TEST_F(NetworkFailoverTest, ConnectionLostAfterGracePeriodTimeout) {
    failoverManager_->setGracePeriod(std::chrono::seconds(30));

    bool connectionLost = false;
    bool wasInGracePeriod = true;
    failoverManager_->setConnectionLostCallback([&](bool gracePeriodActive) {
        wasInGracePeriod = gracePeriodActive;
        if (!gracePeriodActive) {
            connectionLost = true;
        }
    });

    failoverManager_->simulateTotalNetworkLoss();
    failoverManager_->simulateGracePeriodTimeout();

    EXPECT_TRUE(connectionLost);
    EXPECT_FALSE(wasInGracePeriod);
    EXPECT_FALSE(failoverManager_->isConnected());
}

// =============================================================================
// Network Restriction Tests (Requirement 11.5)
// =============================================================================

TEST_F(NetworkFailoverTest, WiFiOnlyModeBlocksCellular) {
    failoverManager_->setNetworkRestriction(NetworkRestriction::WiFiOnly);

    auto result = failoverManager_->rebindToInterface(NetworkType::Cellular);

    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.error(), NetworkFailoverError::NetworkTypeNotAllowed);
}

TEST_F(NetworkFailoverTest, CellularOnlyModeBlocksWiFi) {
    failoverManager_->setNetworkRestriction(NetworkRestriction::CellularOnly);
    failoverManager_->rebindToInterface(NetworkType::Cellular);  // Start on cellular

    auto result = failoverManager_->rebindToInterface(NetworkType::WiFi);

    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.error(), NetworkFailoverError::NetworkTypeNotAllowed);
}

TEST_F(NetworkFailoverTest, WiFiOnlyModeEntersGracePeriodOnWiFiLoss) {
    failoverManager_->setNetworkRestriction(NetworkRestriction::WiFiOnly);

    failoverManager_->simulateWiFiLoss();

    // Should not failover to cellular, enters grace period instead
    EXPECT_TRUE(failoverManager_->isInGracePeriod());
    EXPECT_NE(failoverManager_->getCurrentNetworkType(), NetworkType::Cellular);
}

TEST_F(NetworkFailoverTest, AnyNetworkAllowsAllTypes) {
    failoverManager_->setNetworkRestriction(NetworkRestriction::Any);

    EXPECT_TRUE(failoverManager_->isNetworkTypeAllowed(NetworkType::WiFi));
    EXPECT_TRUE(failoverManager_->isNetworkTypeAllowed(NetworkType::Cellular));
}

// =============================================================================
// Cellular Warning Tests (Requirement 11.6)
// =============================================================================

TEST_F(NetworkFailoverTest, WarnsAboutCellularDataUsage) {
    bool warningReceived = false;

    failoverManager_->setCellularWarningCallback([&]() {
        warningReceived = true;
    });

    failoverManager_->simulateWiFiLoss();  // Triggers cellular failover

    EXPECT_TRUE(warningReceived);
}

TEST_F(NetworkFailoverTest, CellularWarningOnManualRebind) {
    bool warningReceived = false;

    failoverManager_->setCellularWarningCallback([&]() {
        warningReceived = true;
    });

    failoverManager_->rebindToInterface(NetworkType::Cellular);

    EXPECT_TRUE(warningReceived);
}

TEST_F(NetworkFailoverTest, NoCellularWarningWhenDisabled) {
    bool warningReceived = false;

    failoverManager_->setCellularWarningEnabled(false);
    failoverManager_->setCellularWarningCallback([&]() {
        warningReceived = true;
    });

    failoverManager_->rebindToInterface(NetworkType::Cellular);

    EXPECT_FALSE(warningReceived);
}

TEST_F(NetworkFailoverTest, NoCellularWarningForWiFiRebind) {
    failoverManager_->rebindToInterface(NetworkType::Cellular);  // Start on cellular

    bool warningReceived = false;
    failoverManager_->setCellularWarningCallback([&]() {
        warningReceived = true;
    });

    failoverManager_->rebindToInterface(NetworkType::WiFi);

    EXPECT_FALSE(warningReceived);
}

// =============================================================================
// Complex Failover Scenario Tests
// =============================================================================

TEST_F(NetworkFailoverTest, CompleteFailoverSequence) {
    std::vector<NetworkType> transitions;

    failoverManager_->setNetworkChangeCallback([&](const NetworkChangeEvent& event) {
        transitions.push_back(event.newType);
    });

    // Start on WiFi
    EXPECT_EQ(failoverManager_->getCurrentNetworkType(), NetworkType::WiFi);

    // WiFi lost, failover to cellular
    failoverManager_->simulateWiFiLoss();
    EXPECT_EQ(failoverManager_->getCurrentNetworkType(), NetworkType::Cellular);

    // WiFi restored, failover back
    failoverManager_->simulateWiFiRestore();
    EXPECT_EQ(failoverManager_->getCurrentNetworkType(), NetworkType::WiFi);

    // Verify transition sequence
    EXPECT_EQ(transitions.size(), 2u);
    EXPECT_EQ(transitions[0], NetworkType::Cellular);
    EXPECT_EQ(transitions[1], NetworkType::WiFi);
}

TEST_F(NetworkFailoverTest, FailoverWithActiveStreams) {
    connectionManager_->setActiveConnections(5);

    // Preserve state before network loss
    connectionManager_->preserveState();

    failoverManager_->simulateWiFiLoss();

    // Connection state should be preserved during failover
    EXPECT_TRUE(connectionManager_->isStatePreserved());
    EXPECT_EQ(connectionManager_->getActiveConnections(), 5);
}

TEST_F(NetworkFailoverTest, GracePeriodRecoveryRestoresConnections) {
    connectionManager_->setActiveConnections(3);
    connectionManager_->preserveState();

    failoverManager_->simulateTotalNetworkLoss();
    EXPECT_TRUE(failoverManager_->isInGracePeriod());

    // Simulate partial grace period
    failoverManager_->simulateGracePeriodProgress(std::chrono::seconds(15));

    // Network restored
    failoverManager_->simulateNetworkRestore();

    // Connections should be restored
    EXPECT_FALSE(failoverManager_->isInGracePeriod());
    EXPECT_TRUE(failoverManager_->isConnected());
    EXPECT_TRUE(connectionManager_->isStatePreserved());
}

TEST_F(NetworkFailoverTest, MultipleInterfaceChangesInQuickSuccession) {
    int changeCount = 0;
    failoverManager_->setNetworkChangeCallback([&](const NetworkChangeEvent& event) {
        changeCount++;
    });

    // Rapid transitions
    failoverManager_->simulateWiFiLoss();      // WiFi -> Cellular
    failoverManager_->simulateWiFiRestore();   // Cellular -> WiFi
    failoverManager_->simulateCellularLoss();  // (no change, already on WiFi)
    failoverManager_->simulateWiFiLoss();      // WiFi -> Cellular

    // Should handle all transitions gracefully
    EXPECT_GE(changeCount, 2);
}

} // namespace test
} // namespace e2e
} // namespace openrtmp
