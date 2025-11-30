// OpenRTMP - Cross-platform RTMP Server
// Server Lifecycle Manager Implementation
//
// Integrates all components into server lifecycle:
// - Wire configuration loading during initialization
// - Start event loop and connection acceptance on server start
// - Coordinate component shutdown in correct order
// - Validate configuration and report errors on startup
// - Emit server lifecycle events to registered callbacks

#include "openrtmp/api/server_lifecycle.hpp"
#include "openrtmp/core/connection_pool.hpp"
#include "openrtmp/core/auth_service.hpp"
#include "openrtmp/core/tls_service.hpp"
#include "openrtmp/core/shutdown_coordinator.hpp"
#include "openrtmp/core/error_isolation.hpp"
#include "openrtmp/core/metrics_collector.hpp"
#include "openrtmp/core/config_manager.hpp"
#include "openrtmp/streaming/stream_registry.hpp"

// Note: structured_logger.hpp is excluded due to symbol conflicts with config_manager.hpp
// The logger_ member is declared but not used in this implementation

#include <algorithm>
#include <thread>

namespace openrtmp {
namespace api {

// =============================================================================
// Platform Defaults
// =============================================================================

namespace {
    constexpr uint32_t DEFAULT_MAX_CONNECTIONS = 10000;
    constexpr uint32_t DEFAULT_GRACE_PERIOD_MS = 30000;
    constexpr uint16_t DEFAULT_RTMP_PORT = 1935;
    constexpr uint16_t DEFAULT_RTMPS_PORT = 1936;

    // Shutdown order for components (first to shutdown first)
    const std::vector<std::string> SHUTDOWN_ORDER = {
        "ConnectionPool",       // Stop accepting new connections first
        "AuthService",          // Stop auth checks
        "TLSService",           // Stop TLS handling
        "StreamRegistry",       // Clean up streams
        "ShutdownCoordinator",  // Finalize shutdown tasks
        "ErrorIsolation",       // Cleanup error tracking
        "MetricsCollector",     // Final metrics snapshot
        "ConfigManager",        // Last to shutdown
        "Logger"                // Very last
    };
}

// =============================================================================
// ServerLifecycleManager Implementation
// =============================================================================

ServerLifecycleManager::ServerLifecycleManager()
    : state_(LifecycleState::Uninitialized)
    , acceptingConnections_(false)
{
    startTime_ = std::chrono::steady_clock::now();
}

ServerLifecycleManager::ServerLifecycleManager(const LifecycleConfig& config)
    : state_(LifecycleState::Uninitialized)
    , acceptingConnections_(false)
    , config_(config)
{
    startTime_ = std::chrono::steady_clock::now();
}

ServerLifecycleManager::~ServerLifecycleManager() {
    // Ensure clean shutdown
    if (state_.load() == LifecycleState::Running ||
        state_.load() == LifecycleState::Starting) {
        forceShutdown();
    }
}

ServerLifecycleManager::ServerLifecycleManager(ServerLifecycleManager&& other) noexcept
    : state_(other.state_.load())
    , acceptingConnections_(other.acceptingConnections_.load())
{
    std::lock_guard<std::mutex> configLock(other.configMutex_);
    std::lock_guard<std::mutex> componentsLock(other.componentsMutex_);
    std::lock_guard<std::mutex> callbackLock(other.callbackMutex_);

    config_ = std::move(other.config_);
    startTime_ = other.startTime_;

    configManager_ = std::move(other.configManager_);
    connectionPool_ = std::move(other.connectionPool_);
    streamRegistry_ = std::move(other.streamRegistry_);
    authService_ = std::move(other.authService_);
    tlsService_ = std::move(other.tlsService_);
    shutdownCoordinator_ = std::move(other.shutdownCoordinator_);
    errorIsolation_ = std::move(other.errorIsolation_);
    metricsCollector_ = std::move(other.metricsCollector_);
    logger_ = std::move(other.logger_);

    initializedComponents_ = std::move(other.initializedComponents_);

    eventCallback_ = std::move(other.eventCallback_);
    componentShutdownCallback_ = std::move(other.componentShutdownCallback_);
    publisherNotificationCallback_ = std::move(other.publisherNotificationCallback_);

    other.state_ = LifecycleState::Uninitialized;
    other.acceptingConnections_ = false;
}

ServerLifecycleManager& ServerLifecycleManager::operator=(ServerLifecycleManager&& other) noexcept {
    if (this != &other) {
        // Clean up current state
        if (state_.load() == LifecycleState::Running) {
            forceShutdown();
        }

        std::scoped_lock lock(stateMutex_, other.stateMutex_);
        std::lock_guard<std::mutex> configLock(other.configMutex_);
        std::lock_guard<std::mutex> componentsLock(other.componentsMutex_);
        std::lock_guard<std::mutex> callbackLock(other.callbackMutex_);

        state_ = other.state_.load();
        acceptingConnections_ = other.acceptingConnections_.load();

        config_ = std::move(other.config_);
        startTime_ = other.startTime_;

        configManager_ = std::move(other.configManager_);
        connectionPool_ = std::move(other.connectionPool_);
        streamRegistry_ = std::move(other.streamRegistry_);
        authService_ = std::move(other.authService_);
        tlsService_ = std::move(other.tlsService_);
        shutdownCoordinator_ = std::move(other.shutdownCoordinator_);
        errorIsolation_ = std::move(other.errorIsolation_);
        metricsCollector_ = std::move(other.metricsCollector_);
        logger_ = std::move(other.logger_);

        initializedComponents_ = std::move(other.initializedComponents_);

        eventCallback_ = std::move(other.eventCallback_);
        componentShutdownCallback_ = std::move(other.componentShutdownCallback_);
        publisherNotificationCallback_ = std::move(other.publisherNotificationCallback_);

        other.state_ = LifecycleState::Uninitialized;
        other.acceptingConnections_ = false;
    }
    return *this;
}

// =============================================================================
// Lifecycle Operations
// =============================================================================

core::Result<void, LifecycleError> ServerLifecycleManager::initialize() {
    return initialize(LifecycleConfig{});
}

core::Result<void, LifecycleError> ServerLifecycleManager::initialize(const LifecycleConfig& config) {
    // Check state
    {
        std::shared_lock<std::shared_mutex> lock(stateMutex_);
        if (state_.load() != LifecycleState::Uninitialized) {
            return core::Result<void, LifecycleError>::error(
                LifecycleError{LifecycleError::Code::InvalidState,
                              "Server is already initialized"}
            );
        }
    }

    // Validate configuration (Requirement 17.4)
    auto validationResult = validateConfig(config);
    if (validationResult.isError()) {
        // Emit configuration error event
        emitEvent(LifecycleEvent{
            LifecycleEventType::ConfigurationError,
            validationResult.error().message
        });
        return validationResult;
    }

    // Store and apply defaults to configuration
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        config_ = config;
        applyPlatformDefaults(config_);
    }

    // Initialize components
    auto initResult = initializeComponents();
    if (initResult.isError()) {
        return initResult;
    }

    // Update state
    state_.store(LifecycleState::Initialized);
    startTime_ = std::chrono::steady_clock::now();

    // Emit initialized event
    emitEvent(LifecycleEvent{
        LifecycleEventType::ServerInitialized,
        "Server initialized successfully"
    });

    return core::Result<void, LifecycleError>::success();
}

core::Result<void, LifecycleError> ServerLifecycleManager::start() {
    // Check state
    {
        std::shared_lock<std::shared_mutex> lock(stateMutex_);
        auto currentState = state_.load();

        if (currentState == LifecycleState::Running) {
            // Already running - idempotent success
            return core::Result<void, LifecycleError>::success();
        }

        if (currentState != LifecycleState::Initialized &&
            currentState != LifecycleState::Stopped) {
            return core::Result<void, LifecycleError>::error(
                LifecycleError{LifecycleError::Code::InvalidState,
                              "Server must be initialized before starting"}
            );
        }
    }

    // Set starting state
    state_.store(LifecycleState::Starting);

    // Start all components
    auto startResult = startComponents();
    if (startResult.isError()) {
        state_.store(LifecycleState::Stopped);
        return startResult;
    }

    // Enable connection acceptance
    acceptingConnections_.store(true);

    // Update state to running
    state_.store(LifecycleState::Running);

    // Emit started event
    emitEvent(LifecycleEvent{
        LifecycleEventType::ServerStarted,
        "Server started and accepting connections"
    });

    return core::Result<void, LifecycleError>::success();
}

core::Result<void, LifecycleError> ServerLifecycleManager::shutdown(bool graceful) {
    // Check state
    {
        std::shared_lock<std::shared_mutex> lock(stateMutex_);
        auto currentState = state_.load();

        if (currentState == LifecycleState::Stopped ||
            currentState == LifecycleState::Uninitialized) {
            return core::Result<void, LifecycleError>::success();
        }

        if (currentState == LifecycleState::Stopping) {
            // Already shutting down
            return core::Result<void, LifecycleError>::success();
        }
    }

    // Set stopping state
    state_.store(LifecycleState::Stopping);

    // Emit stopping event
    emitEvent(LifecycleEvent{
        LifecycleEventType::ServerStopping,
        graceful ? "Graceful shutdown initiated" : "Immediate shutdown initiated"
    });

    // Stop accepting new connections
    acceptingConnections_.store(false);

    // Notify publishers
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (publisherNotificationCallback_) {
            publisherNotificationCallback_();
        }
    }

    // Wait for grace period if graceful
    if (graceful) {
        uint32_t gracePeriod;
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            gracePeriod = config_.gracePeriodMs;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(gracePeriod));
    }

    // Shutdown all components in correct order
    shutdownComponents(graceful);

    // Update state
    state_.store(LifecycleState::Stopped);

    // Emit stopped event
    emitEvent(LifecycleEvent{
        LifecycleEventType::ServerStopped,
        "Server stopped"
    });

    return core::Result<void, LifecycleError>::success();
}

core::Result<void, LifecycleError> ServerLifecycleManager::forceShutdown() {
    return shutdown(false);
}

// =============================================================================
// State Accessors
// =============================================================================

LifecycleState ServerLifecycleManager::state() const {
    return state_.load();
}

bool ServerLifecycleManager::canAcceptConnections() const {
    return acceptingConnections_.load() && state_.load() == LifecycleState::Running;
}

LifecycleConfig ServerLifecycleManager::getEffectiveConfig() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return config_;
}

LifecycleHealthStatus ServerLifecycleManager::getHealthStatus() const {
    LifecycleHealthStatus status;
    status.state = state_.load();
    status.healthy = (status.state == LifecycleState::Running);

    auto now = std::chrono::steady_clock::now();
    status.uptime = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime_);

    if (status.healthy) {
        status.details = "Server is running normally";
    } else {
        status.details = std::string("Server state: ") + lifecycleStateToString(status.state);
    }

    return status;
}

// =============================================================================
// Component Access
// =============================================================================

bool ServerLifecycleManager::isComponentInitialized(const std::string& componentName) const {
    std::lock_guard<std::mutex> lock(initializedMutex_);
    return std::find(initializedComponents_.begin(), initializedComponents_.end(),
                     componentName) != initializedComponents_.end();
}

std::shared_ptr<core::ConfigManager> ServerLifecycleManager::getConfigManager() const {
    std::lock_guard<std::mutex> lock(componentsMutex_);
    return configManager_;
}

std::shared_ptr<core::ConnectionPool> ServerLifecycleManager::getConnectionPool() const {
    std::lock_guard<std::mutex> lock(componentsMutex_);
    return connectionPool_;
}

std::shared_ptr<streaming::StreamRegistry> ServerLifecycleManager::getStreamRegistry() const {
    std::lock_guard<std::mutex> lock(componentsMutex_);
    return streamRegistry_;
}

std::shared_ptr<core::MetricsCollector> ServerLifecycleManager::getMetricsCollector() const {
    std::lock_guard<std::mutex> lock(componentsMutex_);
    return metricsCollector_;
}

std::shared_ptr<core::ShutdownCoordinator> ServerLifecycleManager::getShutdownCoordinator() const {
    std::lock_guard<std::mutex> lock(componentsMutex_);
    return shutdownCoordinator_;
}

std::shared_ptr<core::ErrorIsolation> ServerLifecycleManager::getErrorIsolation() const {
    std::lock_guard<std::mutex> lock(componentsMutex_);
    return errorIsolation_;
}

// =============================================================================
// Event Callbacks
// =============================================================================

void ServerLifecycleManager::setEventCallback(LifecycleEventCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    eventCallback_ = std::move(callback);
}

void ServerLifecycleManager::setComponentShutdownCallback(ComponentShutdownCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    componentShutdownCallback_ = std::move(callback);
}

void ServerLifecycleManager::setPublisherNotificationCallback(PublisherNotificationCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    publisherNotificationCallback_ = std::move(callback);
}

void ServerLifecycleManager::reportComponentError(const std::string& componentName,
                                                   const std::string& message) {
    emitEvent(LifecycleEvent{
        LifecycleEventType::ComponentError,
        message,
        componentName
    });
}

// =============================================================================
// Internal Methods
// =============================================================================

core::Result<void, LifecycleError> ServerLifecycleManager::validateConfig(
    const LifecycleConfig& config)
{
    // Validate port (Requirement 17.4, 17.5)
    if (config.port == 0) {
        return core::Result<void, LifecycleError>::error(
            LifecycleError{LifecycleError::Code::ConfigurationInvalid,
                          "Invalid port: port cannot be 0"}
        );
    }

    // Validate TLS configuration if enabled
    if (config.enableTLS) {
        if (config.tlsCertPath.empty()) {
            return core::Result<void, LifecycleError>::error(
                LifecycleError{LifecycleError::Code::ConfigurationInvalid,
                              "TLS enabled but certificate path is empty"}
            );
        }
        if (config.tlsKeyPath.empty()) {
            return core::Result<void, LifecycleError>::error(
                LifecycleError{LifecycleError::Code::ConfigurationInvalid,
                              "TLS enabled but key path is empty"}
            );
        }
        if (config.tlsPort == 0) {
            return core::Result<void, LifecycleError>::error(
                LifecycleError{LifecycleError::Code::ConfigurationInvalid,
                              "TLS enabled but RTMPS port is 0"}
            );
        }
        if (config.tlsPort == config.port) {
            return core::Result<void, LifecycleError>::error(
                LifecycleError{LifecycleError::Code::ConfigurationInvalid,
                              "RTMP and RTMPS ports cannot be the same"}
            );
        }
    }

    return core::Result<void, LifecycleError>::success();
}

void ServerLifecycleManager::applyPlatformDefaults(LifecycleConfig& config) {
    // Apply defaults for zero values
    if (config.port == 0) {
        config.port = DEFAULT_RTMP_PORT;
    }
    if (config.maxConnections == 0) {
        config.maxConnections = DEFAULT_MAX_CONNECTIONS;
    }
    if (config.gracePeriodMs == 0) {
        config.gracePeriodMs = DEFAULT_GRACE_PERIOD_MS;
    }
    if (config.enableTLS && config.tlsPort == 0) {
        config.tlsPort = DEFAULT_RTMPS_PORT;
    }
}

core::Result<void, LifecycleError> ServerLifecycleManager::initializeComponents() {
    std::lock_guard<std::mutex> lock(componentsMutex_);

    // Create ConfigManager
    configManager_ = std::make_shared<core::ConfigManager>();
    setComponentInitialized("ConfigManager");

    // Create MetricsCollector
    metricsCollector_ = std::make_shared<core::MetricsCollector>();
    setComponentInitialized("MetricsCollector");

    // Create ErrorIsolation
    errorIsolation_ = std::make_shared<core::ErrorIsolation>();
    setComponentInitialized("ErrorIsolation");

    // Create ShutdownCoordinator
    shutdownCoordinator_ = std::make_shared<core::ShutdownCoordinator>();
    setComponentInitialized("ShutdownCoordinator");

    // Create ConnectionPool with config
    core::ConnectionPoolConfig poolConfig;
    {
        std::lock_guard<std::mutex> configLock(configMutex_);
        poolConfig.maxConnections = config_.maxConnections;
    }
    connectionPool_ = std::make_shared<core::ConnectionPool>(poolConfig);
    setComponentInitialized("ConnectionPool");

    // Create StreamRegistry
    streamRegistry_ = std::make_shared<streaming::StreamRegistry>();
    setComponentInitialized("StreamRegistry");

    // Create AuthService
    authService_ = std::make_shared<core::AuthService>();
    setComponentInitialized("AuthService");

    // Create TLSService
    tlsService_ = std::make_shared<core::TLSService>();
    setComponentInitialized("TLSService");

    return core::Result<void, LifecycleError>::success();
}

core::Result<void, LifecycleError> ServerLifecycleManager::startComponents() {
    // Initialize TLS if enabled
    bool tlsEnabled;
    std::string certPath, keyPath;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        tlsEnabled = config_.enableTLS;
        certPath = config_.tlsCertPath;
        keyPath = config_.tlsKeyPath;
    }

    if (tlsEnabled) {
        std::lock_guard<std::mutex> lock(componentsMutex_);
        if (tlsService_) {
            auto initResult = tlsService_->initialize();
            if (initResult.isSuccess()) {
                auto loadResult = tlsService_->loadCertificate(certPath, keyPath);
                if (loadResult.isError()) {
                    // Log warning but don't fail - TLS will be unavailable
                    emitEvent(LifecycleEvent{
                        LifecycleEventType::ComponentError,
                        "Failed to load TLS certificate: " + loadResult.error().message,
                        "TLSService"
                    });
                }
            } else {
                emitEvent(LifecycleEvent{
                    LifecycleEventType::ComponentError,
                    "Failed to initialize TLS service: " + initResult.error().message,
                    "TLSService"
                });
            }
        }
    }

    // Start metrics collection
    // (MetricsCollector is always ready after construction)

    return core::Result<void, LifecycleError>::success();
}

void ServerLifecycleManager::shutdownComponents(bool graceful) {
    (void)graceful;  // Used for future graceful shutdown behavior

    // Shutdown in defined order
    for (const auto& componentName : SHUTDOWN_ORDER) {
        // Notify about component shutdown
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            if (componentShutdownCallback_) {
                componentShutdownCallback_(componentName);
            }
        }

        // Perform actual shutdown
        {
            std::lock_guard<std::mutex> lock(componentsMutex_);

            if (componentName == "ConnectionPool" && connectionPool_) {
                // Connection pool cleanup
                connectionPool_.reset();
            }
            else if (componentName == "StreamRegistry" && streamRegistry_) {
                streamRegistry_.reset();
            }
            else if (componentName == "AuthService" && authService_) {
                authService_.reset();
            }
            else if (componentName == "TLSService" && tlsService_) {
                if (tlsService_->isInitialized()) {
                    tlsService_->shutdown();
                }
                tlsService_.reset();
            }
            else if (componentName == "ShutdownCoordinator" && shutdownCoordinator_) {
                shutdownCoordinator_.reset();
            }
            else if (componentName == "ErrorIsolation" && errorIsolation_) {
                errorIsolation_.reset();
            }
            else if (componentName == "MetricsCollector" && metricsCollector_) {
                metricsCollector_.reset();
            }
            else if (componentName == "ConfigManager" && configManager_) {
                configManager_.reset();
            }
        }

        setComponentShutdown(componentName);
    }
}

void ServerLifecycleManager::emitEvent(const LifecycleEvent& event) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (eventCallback_) {
        eventCallback_(event);
    }
}

void ServerLifecycleManager::setComponentInitialized(const std::string& componentName) {
    std::lock_guard<std::mutex> lock(initializedMutex_);
    if (std::find(initializedComponents_.begin(), initializedComponents_.end(),
                  componentName) == initializedComponents_.end()) {
        initializedComponents_.push_back(componentName);
    }
}

void ServerLifecycleManager::setComponentShutdown(const std::string& componentName) {
    std::lock_guard<std::mutex> lock(initializedMutex_);
    auto it = std::find(initializedComponents_.begin(), initializedComponents_.end(),
                        componentName);
    if (it != initializedComponents_.end()) {
        initializedComponents_.erase(it);
    }
}

} // namespace api
} // namespace openrtmp
