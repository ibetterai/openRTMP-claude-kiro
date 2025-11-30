// OpenRTMP - Cross-platform RTMP Server
// RTMPServer Public API Implementation
//
// Implements the RTMPServer class with thread-safe operations
// and integration with core services.

#include "openrtmp/api/rtmp_server.hpp"
#include "openrtmp/core/config_manager.hpp"
#include "openrtmp/core/metrics_collector.hpp"
#include "openrtmp/core/shutdown_coordinator.hpp"
#include "openrtmp/core/error_isolation.hpp"
#include "openrtmp/core/auth_service.hpp"
#include "openrtmp/core/connection_pool.hpp"
#include "openrtmp/streaming/stream_registry.hpp"

#include <chrono>
#include <thread>
#include <condition_variable>

namespace openrtmp {
namespace api {

// =============================================================================
// Version Information
// =============================================================================

static constexpr const char* OPENRTMP_VERSION = "0.1.0";

// =============================================================================
// RTMPServer Implementation Class
// =============================================================================

class RTMPServer::Impl {
public:
    Impl()
        : state_(ServerState::Uninitialized)
        , startTime_(std::chrono::steady_clock::now())
    {}

    explicit Impl(const ServerConfig& config)
        : state_(ServerState::Uninitialized)
        , pendingConfig_(config)
        , startTime_(std::chrono::steady_clock::now())
    {}

    ~Impl() {
        // Force stop if still running
        if (state_ == ServerState::Running) {
            forceStopInternal();
        }
    }

    // Non-copyable
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // -------------------------------------------------------------------------
    // Lifecycle Methods
    // -------------------------------------------------------------------------

    core::Result<void, ServerError> initialize(const ServerConfig& config) {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        // Check current state
        if (state_ != ServerState::Uninitialized) {
            return core::Result<void, ServerError>::error(
                ServerError(ServerError::Code::InvalidState,
                           "Server already initialized, cannot initialize again")
            );
        }

        // Validate configuration
        auto validationResult = validateConfig(config);
        if (validationResult.isError()) {
            return validationResult;
        }

        // Store configuration
        config_ = config;

        // Initialize internal services
        initializeServices();

        // Update state
        state_ = ServerState::Initialized;

        return core::Result<void, ServerError>::success();
    }

    core::Result<void, ServerError> start() {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        // Check current state
        if (state_ == ServerState::Running) {
            // Already running - idempotent success
            return core::Result<void, ServerError>::success();
        }

        if (state_ != ServerState::Initialized && state_ != ServerState::Stopped) {
            return core::Result<void, ServerError>::error(
                ServerError(ServerError::Code::InvalidState,
                           "Server must be initialized before starting")
            );
        }

        // Transition to starting
        state_ = ServerState::Starting;

        // Reset metrics on start
        resetMetrics();

        // Start internal services
        startServices();

        // Record start time
        startTime_ = std::chrono::steady_clock::now();

        // Transition to running
        state_ = ServerState::Running;

        // Emit start event
        emitEvent(ServerEventType::ServerStarted, "", "", 0, "Server started");

        return core::Result<void, ServerError>::success();
    }

    core::Result<void, ServerError> stop(bool graceful) {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        if (state_ == ServerState::Stopped || state_ == ServerState::Initialized) {
            // Already stopped or never started - success
            return core::Result<void, ServerError>::success();
        }

        if (state_ != ServerState::Running) {
            return core::Result<void, ServerError>::error(
                ServerError(ServerError::Code::InvalidState,
                           "Server is not running")
            );
        }

        // Transition to stopping
        state_ = ServerState::Stopping;

        if (graceful) {
            // Release lock during grace period
            lock.unlock();
            performGracefulShutdown();
            lock.lock();
        }

        // Stop internal services
        stopServices();

        // Transition to stopped
        state_ = ServerState::Stopped;

        // Emit stop event
        emitEvent(ServerEventType::ServerStopped, "", "", 0, "Server stopped");

        return core::Result<void, ServerError>::success();
    }

    core::Result<void, ServerError> forceStop() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return forceStopInternal();
    }

    // -------------------------------------------------------------------------
    // Runtime Configuration
    // -------------------------------------------------------------------------

    core::Result<void, ServerError> updateConfig(const RuntimeConfigUpdate& update) {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        if (state_ == ServerState::Uninitialized) {
            return core::Result<void, ServerError>::error(
                ServerError(ServerError::Code::InvalidState,
                           "Server must be initialized before updating config")
            );
        }

        // Apply updates (only non-zero values)
        if (update.maxConnections > 0) {
            config_.maxConnections = update.maxConnections;

            // Update connection pool if available
            if (connectionPool_) {
                // Connection pool doesn't support dynamic resize in current impl
                // Store for next restart
            }
        }

        if (update.gracePeriodMs > 0) {
            config_.gracePeriodMs = update.gracePeriodMs;

            // Update shutdown coordinator if available
            if (shutdownCoordinator_) {
                shutdownCoordinator_->setGracePeriodMs(update.gracePeriodMs);
            }
        }

        return core::Result<void, ServerError>::success();
    }

    ServerConfig getConfig() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return config_;
    }

    // -------------------------------------------------------------------------
    // State Accessors
    // -------------------------------------------------------------------------

    ServerState state() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return state_;
    }

    bool isRunning() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return state_ == ServerState::Running;
    }

    // -------------------------------------------------------------------------
    // Metrics
    // -------------------------------------------------------------------------

    ServerMetrics getServerMetrics() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        ServerMetrics metrics;

        // Basic counters
        metrics.activeConnections = activeConnections_;
        metrics.totalConnections = totalConnections_;
        metrics.activeStreams = activeStreams_;
        metrics.totalStreams = totalStreams_;
        metrics.bytesReceived = bytesReceived_;
        metrics.bytesSent = bytesSent_;
        metrics.peakConnections = peakConnections_;
        metrics.peakStreams = peakStreams_;
        metrics.rejectedConnections = rejectedConnections_;
        metrics.authFailures = authFailures_;

        // Calculate uptime
        if (state_ == ServerState::Running) {
            auto now = std::chrono::steady_clock::now();
            auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                now - startTime_
            );
            metrics.uptimeSeconds = static_cast<uint64_t>(uptime.count());
        } else {
            metrics.uptimeSeconds = 0;
        }

        return metrics;
    }

    std::optional<StreamMetrics> getStreamMetrics(const std::string& streamKey) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        if (!streamRegistry_) {
            return std::nullopt;
        }

        // Parse stream key into app/name
        size_t slashPos = streamKey.find('/');
        if (slashPos == std::string::npos) {
            return std::nullopt;
        }

        core::StreamKey key(
            streamKey.substr(0, slashPos),
            streamKey.substr(slashPos + 1)
        );

        auto streamInfo = streamRegistry_->findStream(key);
        if (!streamInfo) {
            return std::nullopt;
        }

        StreamMetrics metrics;
        metrics.streamKey = streamKey;
        metrics.state = streamInfo->state;
        metrics.subscriberCount = static_cast<uint32_t>(streamInfo->subscribers.size());
        metrics.videoCodec = streamInfo->codecInfo.videoCodec;
        metrics.audioCodec = streamInfo->codecInfo.audioCodec;

        // Duration
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(
            now - streamInfo->startedAt
        );
        metrics.durationSeconds = static_cast<uint64_t>(duration.count());

        return metrics;
    }

    // -------------------------------------------------------------------------
    // Health Check
    // -------------------------------------------------------------------------

    HealthStatus getHealthStatus() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        HealthStatus status;

        if (state_ == ServerState::Running) {
            status.healthy = true;
            status.details = "Server running normally";

            // Calculate connection utilization
            if (config_.maxConnections > 0) {
                status.connectionUtilizationPercent =
                    (activeConnections_ * 100) / config_.maxConnections;
            }

            // Check error isolation for memory/health status
            if (errorIsolation_) {
                auto errHealth = errorIsolation_->getHealthStatus();
                if (errHealth.state != core::HealthState::Healthy) {
                    status.healthy = false;
                    status.details = errHealth.details;
                }
            }
        } else {
            status.healthy = false;
            status.details = "Server not running (state: " +
                            std::string(serverStateToString(state_)) + ")";
        }

        return status;
    }

    // -------------------------------------------------------------------------
    // Callbacks
    // -------------------------------------------------------------------------

    void setAuthCallback(AuthCallback callback) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        authCallback_ = std::move(callback);
    }

    void setEventCallback(EventCallback callback) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        eventCallback_ = std::move(callback);
    }

private:
    // -------------------------------------------------------------------------
    // Internal Methods
    // -------------------------------------------------------------------------

    core::Result<void, ServerError> validateConfig(const ServerConfig& config) {
        if (config.port == 0) {
            return core::Result<void, ServerError>::error(
                ServerError(ServerError::Code::InvalidConfiguration,
                           "Invalid port: 0 is not a valid port number")
            );
        }

        if (config.enableTLS) {
            if (config.tlsCertPath.empty() || config.tlsKeyPath.empty()) {
                return core::Result<void, ServerError>::error(
                    ServerError(ServerError::Code::InvalidConfiguration,
                               "TLS enabled but certificate or key path not specified")
                );
            }
        }

        return core::Result<void, ServerError>::success();
    }

    void initializeServices() {
        // Initialize stream registry
        streamRegistry_ = std::make_unique<streaming::StreamRegistry>();

        // Initialize connection pool with platform defaults
        core::ConnectionPoolConfig poolConfig;
        if (config_.maxConnections > 0) {
            poolConfig.maxConnections = config_.maxConnections;
        } else {
            poolConfig = core::ConnectionPoolConfig::platformDefault();
            config_.maxConnections = static_cast<uint32_t>(poolConfig.maxConnections);
        }
        connectionPool_ = std::make_unique<core::ConnectionPool>(poolConfig);

        // Initialize shutdown coordinator
        shutdownCoordinator_ = std::make_unique<core::ShutdownCoordinator>();
        shutdownCoordinator_->setGracePeriodMs(config_.gracePeriodMs);

        // Initialize error isolation
        errorIsolation_ = std::make_unique<core::ErrorIsolation>();

        // Initialize auth service
        core::AuthConfig authConfig;
        authService_ = std::make_unique<core::AuthService>(authConfig);
    }

    void startServices() {
        // Services are initialized, just reset state
        if (shutdownCoordinator_) {
            // Reset shutdown state if needed
        }
    }

    void stopServices() {
        // Cleanup services
        if (connectionPool_) {
            connectionPool_->clear();
        }
        if (streamRegistry_) {
            streamRegistry_->clear();
        }
    }

    void performGracefulShutdown() {
        if (shutdownCoordinator_) {
            // Initiate graceful shutdown
            shutdownCoordinator_->initiateShutdown();

            // Wait for grace period
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.gracePeriodMs)
            );
        }
    }

    core::Result<void, ServerError> forceStopInternal() {
        if (state_ == ServerState::Stopped || state_ == ServerState::Uninitialized) {
            return core::Result<void, ServerError>::success();
        }

        // Force shutdown
        if (shutdownCoordinator_) {
            shutdownCoordinator_->forceShutdown();
        }

        // Stop services
        stopServices();

        // Update state
        state_ = ServerState::Stopped;

        // Emit event (without lock to avoid deadlock)
        emitEventNoLock(ServerEventType::ServerStopped, "", "", 0, "Server force stopped");

        return core::Result<void, ServerError>::success();
    }

    void resetMetrics() {
        activeConnections_ = 0;
        totalConnections_ = 0;
        activeStreams_ = 0;
        totalStreams_ = 0;
        bytesReceived_ = 0;
        bytesSent_ = 0;
        peakConnections_ = 0;
        peakStreams_ = 0;
        rejectedConnections_ = 0;
        authFailures_ = 0;
    }

    void emitEvent(ServerEventType type, const std::string& streamKey,
                   const std::string& clientIP, uint16_t clientPort,
                   const std::string& message) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        emitEventNoLock(type, streamKey, clientIP, clientPort, message);
    }

    void emitEventNoLock(ServerEventType type, const std::string& streamKey,
                         const std::string& clientIP, uint16_t clientPort,
                         const std::string& message) {
        if (eventCallback_) {
            ServerEvent event;
            event.type = type;
            event.timestampMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count()
            );
            event.streamKey = streamKey;
            event.clientIP = clientIP;
            event.clientPort = clientPort;
            event.message = message;

            try {
                eventCallback_(event);
            } catch (...) {
                // Swallow callback exceptions
            }
        }
    }

    // -------------------------------------------------------------------------
    // Member Variables
    // -------------------------------------------------------------------------

    // State
    ServerState state_;
    ServerConfig config_;
    ServerConfig pendingConfig_;

    // Services
    std::unique_ptr<streaming::StreamRegistry> streamRegistry_;
    std::unique_ptr<core::ConnectionPool> connectionPool_;
    std::unique_ptr<core::ShutdownCoordinator> shutdownCoordinator_;
    std::unique_ptr<core::ErrorIsolation> errorIsolation_;
    std::unique_ptr<core::AuthService> authService_;

    // Metrics
    std::atomic<uint32_t> activeConnections_{0};
    std::atomic<uint64_t> totalConnections_{0};
    std::atomic<uint32_t> activeStreams_{0};
    std::atomic<uint64_t> totalStreams_{0};
    std::atomic<uint64_t> bytesReceived_{0};
    std::atomic<uint64_t> bytesSent_{0};
    std::atomic<uint32_t> peakConnections_{0};
    std::atomic<uint32_t> peakStreams_{0};
    std::atomic<uint64_t> rejectedConnections_{0};
    std::atomic<uint64_t> authFailures_{0};

    // Timing
    std::chrono::steady_clock::time_point startTime_;

    // Callbacks
    AuthCallback authCallback_;
    EventCallback eventCallback_;

    // Thread safety
    mutable std::shared_mutex mutex_;
    mutable std::mutex callbackMutex_;
};

// =============================================================================
// RTMPServer Public Methods
// =============================================================================

RTMPServer::RTMPServer()
    : impl_(std::make_unique<Impl>())
{}

RTMPServer::RTMPServer(const ServerConfig& config)
    : impl_(std::make_unique<Impl>(config))
{}

RTMPServer::~RTMPServer() = default;

RTMPServer::RTMPServer(RTMPServer&& other) noexcept = default;

RTMPServer& RTMPServer::operator=(RTMPServer&& other) noexcept = default;

core::Result<void, ServerError> RTMPServer::initialize(const ServerConfig& config) {
    return impl_->initialize(config);
}

core::Result<void, ServerError> RTMPServer::start() {
    return impl_->start();
}

core::Result<void, ServerError> RTMPServer::stop(bool graceful) {
    return impl_->stop(graceful);
}

core::Result<void, ServerError> RTMPServer::forceStop() {
    return impl_->forceStop();
}

core::Result<void, ServerError> RTMPServer::updateConfig(const RuntimeConfigUpdate& update) {
    return impl_->updateConfig(update);
}

ServerConfig RTMPServer::getConfig() const {
    return impl_->getConfig();
}

ServerState RTMPServer::state() const {
    return impl_->state();
}

bool RTMPServer::isRunning() const {
    return impl_->isRunning();
}

ServerMetrics RTMPServer::getServerMetrics() const {
    return impl_->getServerMetrics();
}

std::optional<StreamMetrics> RTMPServer::getStreamMetrics(const std::string& streamKey) const {
    return impl_->getStreamMetrics(streamKey);
}

HealthStatus RTMPServer::getHealthStatus() const {
    return impl_->getHealthStatus();
}

void RTMPServer::setAuthCallback(AuthCallback callback) {
    impl_->setAuthCallback(std::move(callback));
}

void RTMPServer::setEventCallback(EventCallback callback) {
    impl_->setEventCallback(std::move(callback));
}

std::string RTMPServer::getVersion() {
    return OPENRTMP_VERSION;
}

} // namespace api
} // namespace openrtmp
