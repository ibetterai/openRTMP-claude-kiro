// OpenRTMP - Cross-platform RTMP Server
// RTMPServer Public API Implementation
//
// Implements the RTMPServer class with thread-safe operations
// and integration with core services and platform networking.

#include "openrtmp/api/rtmp_server.hpp"
#include "openrtmp/core/config_manager.hpp"
#include "openrtmp/core/metrics_collector.hpp"
#include "openrtmp/core/shutdown_coordinator.hpp"
#include "openrtmp/core/error_isolation.hpp"
#include "openrtmp/core/auth_service.hpp"
#include "openrtmp/core/connection_pool.hpp"
#include "openrtmp/streaming/stream_registry.hpp"
#include "openrtmp/streaming/gop_buffer.hpp"
#include "openrtmp/protocol/handshake_handler.hpp"
#include "openrtmp/protocol/chunk_parser.hpp"
#include "openrtmp/protocol/command_handler.hpp"
#include "openrtmp/protocol/amf_codec.hpp"
#include "openrtmp/core/buffer.hpp"

// Platform-specific network PAL
#if defined(__APPLE__)
#include "openrtmp/pal/darwin/darwin_network_pal.hpp"
using PlatformNetworkPAL = openrtmp::pal::darwin::DarwinNetworkPAL;
#endif

#include <chrono>
#include <thread>
#include <condition_variable>
#include <unordered_map>
#include <iostream>
#include <iomanip>

namespace openrtmp {
namespace api {

// =============================================================================
// Version Information
// =============================================================================

static constexpr const char* OPENRTMP_VERSION = "0.1.0";

// =============================================================================
// Stream Data - Per-stream metadata and sequence headers
// =============================================================================

struct StreamData {
    std::string streamKey;
    std::vector<uint8_t> videoSequenceHeader;  // AVC sequence header (for H.264)
    std::vector<uint8_t> audioSequenceHeader;  // AAC sequence header
    std::vector<uint8_t> metadata;             // @setDataFrame onMetaData
    streaming::GOPBuffer gopBuffer;            // Full GOP buffer for instant playback
    uint32_t videoTimestamp = 0;               // Last video timestamp
    uint32_t audioTimestamp = 0;               // Last audio timestamp
    bool hasVideoHeader = false;
    bool hasAudioHeader = false;
    bool hasMetadata = false;

    StreamData() : gopBuffer(streaming::GOPBufferConfig{
        std::chrono::milliseconds{5000},  // 5 seconds buffer for instant playback
        20 * 1024 * 1024                   // 20MB max (larger for better GOP coverage)
    }) {}
};

// =============================================================================
// Connection Context - Per-connection state
// =============================================================================

struct ConnectionContext {
    uint64_t id;
    pal::SocketHandle socket;
    std::string clientIP;
    uint16_t clientPort;

    // Protocol handlers
    std::unique_ptr<protocol::HandshakeHandler> handshakeHandler;
    std::unique_ptr<protocol::ChunkParser> chunkParser;
    // CommandHandler needs StreamRegistry and AMFCodec - defer creation
    protocol::SessionContext sessionContext;

    // Connection state
    enum class State {
        Handshaking,
        Connected,
        Publishing,
        Subscribing,
        Disconnecting
    };
    State state = State::Handshaking;

    // Buffers
    core::Buffer readBuffer;
    std::vector<uint8_t> pendingWrite;

    // Stream info
    std::string streamKey;

    // Subscriber initialization state - WAIT FOR KEYFRAME approach
    // Instead of buffering GOP, we wait for the next keyframe from publisher
    // This is simpler and more reliable - no risk of corrupted data
    bool initialDataSent = false;    // Sequence headers and metadata sent
    bool waitingForKeyframe = true;  // Wait until first keyframe before relaying video
    bool receivedFirstKeyframe = false;  // Has received first keyframe, can relay all frames now

    // Chunk sizes
    size_t serverChunkSize = 128;  // Default RTMP chunk size

    // RTMP data accumulation buffer tracking
    size_t rtmpBufferSize = 0;     // Amount of valid data in readBuffer
    size_t rtmpReadOffset = 0;     // Current read offset for async operations
    core::Buffer rtmpOffsetBuffer; // Buffer for offset reads (persists during async read)
    static constexpr size_t READ_BUFFER_CAPACITY = 8192;  // Fixed buffer capacity

    ConnectionContext(uint64_t connId, pal::SocketHandle sock,
                      const std::string& ip, uint16_t port)
        : id(connId), socket(sock), clientIP(ip), clientPort(port)
        , handshakeHandler(std::make_unique<protocol::HandshakeHandler>())
        , chunkParser(std::make_unique<protocol::ChunkParser>())
        , readBuffer(READ_BUFFER_CAPACITY)  // 8KB initial read buffer
        , rtmpOffsetBuffer(READ_BUFFER_CAPACITY)  // Same size as readBuffer
    {
        sessionContext.connectionId = static_cast<ConnectionId>(connId);
    }
};

// =============================================================================
// RTMPServer Implementation Class
// =============================================================================

class RTMPServer::Impl {
public:
    Impl()
        : state_(ServerState::Uninitialized)
        , nextConnectionId_(1)
        , startTime_(std::chrono::steady_clock::now())
    {}

    explicit Impl(const ServerConfig& config)
        : state_(ServerState::Uninitialized)
        , pendingConfig_(config)
        , nextConnectionId_(1)
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

#if defined(__APPLE__)
        // Initialize network PAL
        networkPal_ = std::make_unique<PlatformNetworkPAL>();
        auto palResult = networkPal_->initialize();
        if (palResult.isError()) {
            return core::Result<void, ServerError>::error(
                ServerError(ServerError::Code::StartFailed,
                           "Failed to initialize network: " + palResult.error().message)
            );
        }
#endif

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

#if defined(__APPLE__)
        // Create server socket and bind to port
        pal::ServerOptions serverOpts;
        serverOpts.reuseAddr = true;
        serverOpts.reusePort = true;
        serverOpts.backlog = 128;

        std::string bindAddr = config_.bindAddress.empty() ? "0.0.0.0" : config_.bindAddress;
        auto serverResult = networkPal_->createServer(bindAddr, config_.port, serverOpts);

        if (serverResult.isError()) {
            state_ = ServerState::Initialized;
            return core::Result<void, ServerError>::error(
                ServerError(ServerError::Code::BindFailed,
                           "Failed to bind to port " + std::to_string(config_.port) +
                           ": " + serverResult.error().message)
            );
        }

        serverSocket_ = serverResult.value();

        // Start accepting connections
        startAccepting();

        // Start event loop in background thread
        eventLoopThread_ = std::thread([this]() {
            networkPal_->runEventLoop();
        });
#endif

        // Record start time
        startTime_ = std::chrono::steady_clock::now();

        // Transition to running
        state_ = ServerState::Running;

        // Emit start event
        emitEvent(ServerEventType::ServerStarted, "", "", 0, "Server started on port " + std::to_string(config_.port));

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

#if defined(__APPLE__)
        // Stop network event loop
        if (networkPal_) {
            networkPal_->stopEventLoop();
        }

        // Wait for event loop thread to finish
        if (eventLoopThread_.joinable()) {
            lock.unlock();
            eventLoopThread_.join();
            lock.lock();
        }

        // Close all connections
        closeAllConnections();

        // Close server socket
        if (serverSocket_.handle.value != 0) {
            networkPal_->closeSocket(serverSocket_.handle);
            serverSocket_.handle.value = 0;
        }
#endif

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
        // Initialize stream registry (shared_ptr for use by CommandHandler)
        streamRegistry_ = std::make_shared<streaming::StreamRegistry>();

        // Initialize AMF codec (shared_ptr for use by CommandHandler)
        amfCodec_ = std::make_shared<protocol::AMFCodec>();

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

#if defined(__APPLE__)
        // Stop network event loop
        if (networkPal_) {
            networkPal_->stopEventLoop();
        }

        // Wait for event loop thread
        if (eventLoopThread_.joinable()) {
            eventLoopThread_.join();
        }

        // Close all connections
        closeAllConnections();

        // Close server socket
        if (serverSocket_.handle.value != 0) {
            networkPal_->closeSocket(serverSocket_.handle);
            serverSocket_.handle.value = 0;
        }
#endif

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

    // -------------------------------------------------------------------------
    // Connection Management (Platform-specific)
    // -------------------------------------------------------------------------

#if defined(__APPLE__)
    void startAccepting() {
        networkPal_->asyncAccept(serverSocket_,
            [this](core::Result<pal::SocketHandle, pal::NetworkError> result) {
                handleAccept(std::move(result));
            }
        );
    }

    void handleAccept(core::Result<pal::SocketHandle, pal::NetworkError> result) {
        if (state_ != ServerState::Running) {
            return;
        }

        if (result.isError()) {
            // Log error but continue accepting
            emitEvent(ServerEventType::Error, "", "", 0,
                     "Accept error: " + result.error().message);
            // Continue accepting
            startAccepting();
            return;
        }

        pal::SocketHandle clientSocket = result.value();

        // Check connection limit
        if (config_.maxConnections > 0 && activeConnections_ >= config_.maxConnections) {
            networkPal_->closeSocket(clientSocket);
            rejectedConnections_++;
            emitEvent(ServerEventType::ConnectionLimitReached, "",
                     "", 0, "Connection limit reached");
            startAccepting();
            return;
        }

        // Get peer address info (we don't have this easily from SocketHandle, use placeholder)
        std::string clientIP = "unknown";
        uint16_t clientPort = 0;

        // Create connection context
        uint64_t connId = nextConnectionId_++;
        auto ctx = std::make_shared<ConnectionContext>(
            connId, clientSocket, clientIP, clientPort
        );

        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            connections_[connId] = ctx;
        }

        // Update metrics
        activeConnections_++;
        totalConnections_++;
        if (activeConnections_ > peakConnections_) {
            peakConnections_ = activeConnections_.load();
        }

        // Emit connect event
        emitEvent(ServerEventType::ClientConnected, "",
                 clientIP, clientPort, "Client connected");

        // Start reading from connection (handshake phase)
        startReading(ctx);

        // Continue accepting new connections
        startAccepting();
    }

    void startReading(std::shared_ptr<ConnectionContext> ctx) {
        // For RTMP data, read into buffer after existing unconsumed data
        // For handshaking, always read from the beginning
        size_t offset = 0;
        // Use the fixed buffer capacity - the buffer may have been resized by the network PAL
        size_t maxRead = ConnectionContext::READ_BUFFER_CAPACITY;

        std::cerr << "[DEBUG] startReading: state=" << static_cast<int>(ctx->state)
                  << " rtmpBufferSize=" << ctx->rtmpBufferSize << std::endl;

        // Restore buffer to full capacity before reading (network PAL may have resized it)
        if (ctx->readBuffer.size() < ConnectionContext::READ_BUFFER_CAPACITY) {
            ctx->readBuffer.resize(ConnectionContext::READ_BUFFER_CAPACITY);
        }

        if (ctx->state != ConnectionContext::State::Handshaking) {
            offset = ctx->rtmpBufferSize;
            maxRead = ConnectionContext::READ_BUFFER_CAPACITY - offset;
            if (maxRead == 0) {
                // Buffer is full - should not happen in normal operation
                closeConnection(ctx, "RTMP buffer overflow");
                return;
            }
        }

        std::cerr << "[DEBUG] startReading: offset=" << offset << " maxRead=" << maxRead << std::endl;

        // For RTMP state with accumulated data, we need to read at an offset
        // We use a separate "read offset buffer" that points to the correct position
        if (offset > 0) {
            // Create a separate buffer at the offset position in ctx->rtmpReadBuffer
            // This buffer is kept alive by the context
            ctx->rtmpReadOffset = offset;
            // Restore offset buffer to full capacity (network PAL may have resized it)
            if (ctx->rtmpOffsetBuffer.size() < ConnectionContext::READ_BUFFER_CAPACITY) {
                ctx->rtmpOffsetBuffer.resize(ConnectionContext::READ_BUFFER_CAPACITY);
            }
            networkPal_->asyncRead(ctx->socket, ctx->rtmpOffsetBuffer, maxRead,
                [this, ctx, offset, maxRead](core::Result<size_t, pal::NetworkError> result) {
                    if (result.isSuccess()) {
                        // Copy the data from rtmpOffsetBuffer to readBuffer at the correct offset
                        size_t bytesRead = result.value();
                        std::memcpy(ctx->readBuffer.data() + offset,
                                   ctx->rtmpOffsetBuffer.data(),
                                   bytesRead);
                    }
                    handleRead(ctx, std::move(result));
                }
            );
        } else {
            // For handshaking or fresh RTMP reads, use the main buffer directly
            networkPal_->asyncRead(ctx->socket, ctx->readBuffer, maxRead,
                [this, ctx](core::Result<size_t, pal::NetworkError> result) {
                    handleRead(ctx, std::move(result));
                }
            );
        }
    }

    void handleRead(std::shared_ptr<ConnectionContext> ctx,
                   core::Result<size_t, pal::NetworkError> result) {
        if (state_ != ServerState::Running) {
            std::cerr << "[DEBUG] handleRead: server not running" << std::endl;
            return;
        }

        if (result.isError()) {
            // Connection error - close it
            std::cerr << "[DEBUG] handleRead: read error: " << result.error().message << std::endl;
            closeConnection(ctx, "Read error: " + result.error().message);
            return;
        }

        size_t bytesRead = result.value();
        std::cerr << "[DEBUG] handleRead: bytesRead=" << bytesRead << " rtmpBufferSize=" << ctx->rtmpBufferSize << std::endl;
        if (bytesRead == 0) {
            // Connection closed by peer
            std::cerr << "[DEBUG] handleRead: bytesRead=0, closing connection" << std::endl;
            closeConnection(ctx, "Connection closed by peer");
            return;
        }

        bytesReceived_ += bytesRead;

        // Process data based on connection state
        if (ctx->state == ConnectionContext::State::Handshaking) {
            processHandshake(ctx, bytesRead);
        } else {
            // For RTMP data, accumulate in the buffer
            ctx->rtmpBufferSize += bytesRead;
            processRTMPData(ctx);
        }
    }

    void processHandshake(std::shared_ptr<ConnectionContext> ctx, size_t bytesRead) {
        auto handshakeResult = ctx->handshakeHandler->processData(
            ctx->readBuffer.data(), bytesRead
        );

        if (!handshakeResult.success) {
            closeConnection(ctx, "Handshake failed: " +
                          (handshakeResult.error ? handshakeResult.error->message : "unknown"));
            return;
        }

        // Track how many bytes were consumed by handshake
        size_t bytesConsumed = handshakeResult.bytesConsumed;
        size_t leftoverBytes = bytesRead - bytesConsumed;

        // Check if we have response data to send
        auto responseData = ctx->handshakeHandler->getResponseData();
        if (!responseData.empty()) {
            // Send S0+S1+S2 response
            core::Buffer sendBuffer(responseData.data(), responseData.size());

            // Capture leftover bytes info for the callback
            size_t capturedLeftover = leftoverBytes;

            networkPal_->asyncWrite(ctx->socket, sendBuffer,
                [this, ctx, capturedLeftover, bytesConsumed](core::Result<size_t, pal::NetworkError> writeResult) {
                    if (writeResult.isError()) {
                        closeConnection(ctx, "Write error during handshake");
                        return;
                    }
                    bytesSent_ += writeResult.value();

                    // Check if handshake just completed and we have leftover data
                    if (ctx->handshakeHandler->isComplete() && capturedLeftover > 0) {
                        // Transition to connected state
                        ctx->state = ConnectionContext::State::Connected;
                        ctx->sessionContext.state = protocol::SessionState::Connected;

                        // Move leftover bytes to the beginning of read buffer
                        std::memmove(ctx->readBuffer.data(),
                                    ctx->readBuffer.data() + bytesConsumed,
                                    capturedLeftover);

                        // Set the buffer size for RTMP data processing
                        ctx->rtmpBufferSize = capturedLeftover;

                        // Process leftover bytes as RTMP data
                        processRTMPData(ctx);
                    } else {
                        // Continue reading
                        startReading(ctx);
                    }
                }
            );
            return;
        }

        // Check if handshake is complete
        if (ctx->handshakeHandler->isComplete()) {
            ctx->state = ConnectionContext::State::Connected;
            ctx->sessionContext.state = protocol::SessionState::Connected;

            // Check for leftover bytes after handshake (e.g., RTMP connect command)
            if (leftoverBytes > 0) {
                // Move leftover bytes to the beginning of read buffer
                std::memmove(ctx->readBuffer.data(),
                            ctx->readBuffer.data() + bytesConsumed,
                            leftoverBytes);

                // Set the buffer size for RTMP data processing
                ctx->rtmpBufferSize = leftoverBytes;

                // Process leftover bytes as RTMP data
                processRTMPData(ctx);
            } else {
                // Continue reading RTMP data
                startReading(ctx);
            }
        } else {
            // Need more data
            startReading(ctx);
        }
    }

    void processRTMPData(std::shared_ptr<ConnectionContext> ctx) {
        // DEBUG: Log data received
        std::cerr << "[DEBUG] processRTMPData: bufferSize=" << ctx->rtmpBufferSize
                  << " state=" << static_cast<int>(ctx->state) << std::endl;
        std::cerr << "[DEBUG] First 16 bytes: ";
        for (size_t i = 0; i < std::min(ctx->rtmpBufferSize, size_t(16)); i++) {
            std::cerr << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(ctx->readBuffer[i]) << " ";
        }
        std::cerr << std::dec << std::endl;
        std::cerr.flush();

        // Feed accumulated data to chunk parser
        auto parseResult = ctx->chunkParser->parse(ctx->readBuffer.data(), ctx->rtmpBufferSize);

        if (parseResult.error.has_value()) {
            closeConnection(ctx, "RTMP parse error: " + parseResult.error->message);
            return;
        }

        // DEBUG: Log parse result
        std::cerr << "[DEBUG] Parse consumed=" << parseResult.bytesConsumed << std::endl;
        std::cerr.flush();

        // Compact the buffer: move unconsumed bytes to the beginning
        size_t bytesConsumed = parseResult.bytesConsumed;
        size_t remainingBytes = ctx->rtmpBufferSize - bytesConsumed;
        if (bytesConsumed > 0 && remainingBytes > 0) {
            std::memmove(ctx->readBuffer.data(),
                        ctx->readBuffer.data() + bytesConsumed,
                        remainingBytes);
        }
        ctx->rtmpBufferSize = remainingBytes;

        // Get completed chunks and process them
        auto completedChunks = ctx->chunkParser->getCompletedChunks();

        // DEBUG: Log completed chunks
        std::cerr << "[DEBUG] Completed chunks: " << completedChunks.size() << std::endl;
        std::cerr.flush();

        for (const auto& chunk : completedChunks) {
            processChunk(ctx, chunk);
        }

        // Continue reading
        startReading(ctx);
    }

    void processChunk(std::shared_ptr<ConnectionContext> ctx,
                     const protocol::ChunkData& chunk) {
        // DEBUG: Log every chunk
        std::cerr << "[DEBUG] processChunk: typeId=" << static_cast<int>(chunk.messageTypeId)
                  << " csid=" << chunk.chunkStreamId
                  << " msid=" << chunk.messageStreamId
                  << " len=" << chunk.payload.size() << std::endl;
        std::cerr.flush();

        // Handle protocol control messages
        if (chunk.messageTypeId <= 6) {
            // Protocol control message - handled by chunk parser
            std::cerr << "[DEBUG] Skipping protocol control message (type " << static_cast<int>(chunk.messageTypeId) << ")" << std::endl;
            std::cerr.flush();
            return;
        }

        // Handle command messages (type 17 = AMF3, type 20 = AMF0)
        if (chunk.messageTypeId == 17 || chunk.messageTypeId == 20) {
            std::cerr << "[DEBUG] Processing command message (type " << static_cast<int>(chunk.messageTypeId) << ")" << std::endl;
            std::cerr.flush();
            processCommandMessage(ctx, chunk);
        }

        // Handle data messages (type 18 = AMF0 Data, contains metadata like @setDataFrame)
        if (chunk.messageTypeId == 18) {
            processDataMessage(ctx, chunk);
        }

        // Handle audio (type 8) and video (type 9) data
        if (chunk.messageTypeId == 8 || chunk.messageTypeId == 9) {
            processMediaData(ctx, chunk);
        }
    }

    void processDataMessage(std::shared_ptr<ConnectionContext> ctx,
                           const protocol::ChunkData& chunk) {
        // Data messages contain metadata like @setDataFrame onMetaData
        if (ctx->state != ConnectionContext::State::Publishing || ctx->streamKey.empty()) {
            return;
        }

        std::cerr << "[DEBUG] processDataMessage: type=" << static_cast<int>(chunk.messageTypeId)
                  << " len=" << chunk.payload.size() << std::endl;

        // Store the metadata for this stream
        std::lock_guard<std::mutex> lock(streamDataMutex_);
        auto& streamData = streamDataMap_[ctx->streamKey];
        streamData.streamKey = ctx->streamKey;
        streamData.metadata = chunk.payload;
        streamData.hasMetadata = true;

        std::cerr << "[DEBUG] Stored metadata for stream: " << ctx->streamKey
                  << " size=" << chunk.payload.size() << std::endl;

        // Relay metadata to all subscribers
        relayToSubscribers(ctx->streamKey, chunk);
    }

    void processMediaData(std::shared_ptr<ConnectionContext> ctx,
                         const protocol::ChunkData& chunk) {
        if (ctx->state != ConnectionContext::State::Publishing || ctx->streamKey.empty()) {
            return;
        }

        bool isVideo = (chunk.messageTypeId == 9);
        bool isAudio = (chunk.messageTypeId == 8);
        bool isKeyframe = false;

        // Check for sequence headers and push frames to GOP buffer
        {
            std::lock_guard<std::mutex> lock(streamDataMutex_);
            auto& streamData = streamDataMap_[ctx->streamKey];
            streamData.streamKey = ctx->streamKey;

            if (isVideo && !chunk.payload.empty()) {
                // Check if this is a video sequence header (AVC/H.264)
                // FLV video tag: first byte contains frame type (upper 4 bits) and codec ID (lower 4 bits)
                // For H.264: codec ID = 7 (0x07)
                // For AVC sequence header: first byte & 0x0F == 7 (H.264) and second byte == 0 (AVC sequence header)
                uint8_t firstByte = chunk.payload[0];
                uint8_t codecId = firstByte & 0x0F;
                uint8_t frameType = (firstByte >> 4) & 0x0F;  // 1=keyframe, 2=interframe

                if (codecId == 7 && chunk.payload.size() > 1) {
                    // H.264/AVC
                    uint8_t avcPacketType = chunk.payload[1];
                    if (avcPacketType == 0) {
                        // AVC sequence header - store it separately (not in GOP buffer)
                        streamData.videoSequenceHeader = chunk.payload;
                        streamData.hasVideoHeader = true;
                        std::cerr << "[DEBUG] Stored video sequence header for stream: " << ctx->streamKey
                                  << " size=" << chunk.payload.size() << std::endl;
                    } else if (avcPacketType == 1) {
                        // AVC NALU (actual video data) - push to GOP buffer
                        isKeyframe = (frameType == 1);

                        streaming::BufferedFrame frame;
                        frame.type = MediaType::Video;
                        frame.timestamp = chunk.timestamp;
                        frame.data = chunk.payload;
                        frame.isKeyframe = isKeyframe;
                        streamData.gopBuffer.push(frame);

                        if (isKeyframe) {
                            std::cerr << "[DEBUG] Pushed keyframe to GOP buffer: " << ctx->streamKey
                                      << " size=" << chunk.payload.size()
                                      << " ts=" << chunk.timestamp
                                      << " gopFrames=" << streamData.gopBuffer.getFrameCount() << std::endl;
                        }
                    }
                }
                streamData.videoTimestamp = chunk.timestamp;
            }

            if (isAudio && !chunk.payload.empty()) {
                // Check if this is an audio sequence header (AAC)
                // FLV audio tag: first byte contains sound format (upper 4 bits) and other info
                // For AAC: format = 10 (0x0A in upper 4 bits = 0xA0)
                // For AAC sequence header: first byte >> 4 == 10 and second byte == 0
                uint8_t firstByte = chunk.payload[0];
                uint8_t soundFormat = (firstByte >> 4) & 0x0F;
                if (soundFormat == 10 && chunk.payload.size() > 1) {
                    // AAC
                    uint8_t aacPacketType = chunk.payload[1];
                    if (aacPacketType == 0) {
                        // AAC sequence header - store it separately
                        streamData.audioSequenceHeader = chunk.payload;
                        streamData.hasAudioHeader = true;
                        std::cerr << "[DEBUG] Stored audio sequence header for stream: " << ctx->streamKey
                                  << " size=" << chunk.payload.size() << std::endl;
                    } else {
                        // AAC raw audio data - push to GOP buffer
                        streaming::BufferedFrame frame;
                        frame.type = MediaType::Audio;
                        frame.timestamp = chunk.timestamp;
                        frame.data = chunk.payload;
                        frame.isKeyframe = false;  // Audio frames aren't keyframes
                        streamData.gopBuffer.push(frame);
                    }
                }
                streamData.audioTimestamp = chunk.timestamp;
            }
        }

        // Relay the media data to all subscribers
        relayToSubscribers(ctx->streamKey, chunk);
    }

    void relayToSubscribers(const std::string& streamKey,
                           const protocol::ChunkData& chunk) {
        // Get subscriber list from stream registry
        core::StreamKey key("live", streamKey);
        auto streamInfo = streamRegistry_->findStream(key);
        if (!streamInfo) {
            return;
        }
        if (streamInfo->subscribers.empty()) {
            return;
        }

        // Detect if this is a video keyframe
        bool isVideo = (chunk.messageTypeId == 9);
        bool isKeyframe = false;

        if (isVideo && !chunk.payload.empty()) {
            uint8_t firstByte = chunk.payload[0];
            uint8_t frameType = (firstByte >> 4) & 0x0F;
            isKeyframe = (frameType == 1);  // 1 = keyframe
        }

        // Use chunk stream ID 6 for video, 4 for audio (common convention)
        uint32_t chunkStreamId = isVideo ? 6 : 4;
        if (chunk.messageTypeId == 18) {
            chunkStreamId = 5;  // Data messages
        }

        // Serialize chunk ONCE with original timestamp (no rebasing - simpler and safer)
        std::vector<uint8_t> chunkData = serializeChunk(
            chunkStreamId,
            chunk.timestamp,
            chunk.payload.size(),
            chunk.messageTypeId,
            1,  // message stream ID 1 for subscribers
            chunk.payload,
            4096  // Must match the Set Chunk Size sent in sendInitialStreamData()
        );

        // Send to each subscriber using WAIT FOR KEYFRAME approach
        for (const auto& subscriberId : streamInfo->subscribers) {
            std::shared_ptr<ConnectionContext> subscriberCtx;
            {
                std::lock_guard<std::mutex> lock(connectionsMutex_);
                auto it = connections_.find(subscriberId);
                if (it != connections_.end()) {
                    subscriberCtx = it->second;
                }
            }

            if (!subscriberCtx || subscriberCtx->state != ConnectionContext::State::Subscribing) {
                continue;
            }

            // WAIT FOR KEYFRAME logic (nginx-rtmp style):
            // Drop BOTH audio AND video until first video keyframe arrives
            // This prevents A/V desync and ensures clean decoder state
            // Reference: nginx-rtmp-module wait_key with interleave mode
            if (subscriberCtx->waitingForKeyframe) {
                if (isVideo && isKeyframe) {
                    // First keyframe received - start relaying BOTH audio and video
                    subscriberCtx->waitingForKeyframe = false;
                    subscriberCtx->receivedFirstKeyframe = true;
                    std::cerr << "[RELAY] Subscriber " << subscriberId
                              << " received first keyframe, starting A/V relay" << std::endl;
                } else {
                    // Still waiting for keyframe - skip ALL data (audio AND video)
                    // This matches nginx-rtmp behavior with wait_key + interleave
                    continue;
                }
            }

            if (!subscriberCtx->initialDataSent) {
                // Don't relay until sendInitialStreamData has been called
                continue;
            }

            core::Buffer sendBuffer(chunkData.data(), chunkData.size());
            networkPal_->asyncWrite(subscriberCtx->socket, sendBuffer,
                [this](core::Result<size_t, pal::NetworkError> writeResult) {
                    if (writeResult.isSuccess()) {
                        bytesSent_ += writeResult.value();
                    }
                }
            );
        }
    }

    void processCommandMessage(std::shared_ptr<ConnectionContext> ctx,
                               const protocol::ChunkData& chunk) {
        // Determine AMF version
        protocol::AMFVersion version = (chunk.messageTypeId == 17)
            ? protocol::AMFVersion::AMF3
            : protocol::AMFVersion::AMF0;

        // For AMF3 command messages, skip the first byte (always 0x00)
        const uint8_t* payload = chunk.payload.data();
        size_t payloadLen = chunk.payload.size();
        if (version == protocol::AMFVersion::AMF3 && payloadLen > 0) {
            payload++;
            payloadLen--;
        }

        // Decode all AMF values from the command message
        auto decodeResult = amfCodec_->decodeAll(version, payload, payloadLen);
        if (decodeResult.isError()) {
            // Decoding failed - close connection
            closeConnection(ctx, "AMF decode error: " + decodeResult.error().message);
            return;
        }

        auto& values = decodeResult.value();
        if (values.size() < 3) {
            // Command messages require at least: name, transactionId, commandObject
            return;
        }

        // Parse RTMPCommand structure
        protocol::RTMPCommand command;
        command.name = values[0].asString();
        command.transactionId = values[1].asNumber();
        command.commandObject = values[2];

        // Copy additional arguments
        for (size_t i = 3; i < values.size(); i++) {
            command.args.push_back(values[i]);
        }

        // DEBUG: Log command received
        std::cerr << "[DEBUG] Received command: '" << command.name << "' txid=" << command.transactionId
                  << " msid=" << chunk.messageStreamId << " csid=" << chunk.chunkStreamId
                  << " args=" << command.args.size() << std::endl;

        // Create CommandHandler for this connection
        protocol::CommandHandler commandHandler(streamRegistry_, amfCodec_);

        // Process the command
        auto result = commandHandler.processCommand(command, ctx->sessionContext);
        if (result.isError()) {
            // Command processing failed - send error response if applicable
            std::cerr << "[DEBUG] Command '" << command.name << "' FAILED: " << result.error().description << std::endl;
            return;
        }

        // DEBUG: Log session state after command
        std::cerr << "[DEBUG] After '" << command.name << "': sessionState=" << static_cast<int>(ctx->sessionContext.state)
                  << " streamId=" << ctx->sessionContext.streamId
                  << " streamKey=" << ctx->sessionContext.streamKey
                  << " responses=" << result.value().responses.size() << std::endl;

        // Build combined buffer for all messages to send atomically
        std::vector<uint8_t> combinedBuffer;

        // For connect command, add protocol control messages first
        bool isConnectCommand = (command.name == "connect");
        if (isConnectCommand) {
            // Window Acknowledgement Size (type 5) - 2500000 bytes
            auto windowAckData = buildProtocolControlMessage(5, 2500000);
            combinedBuffer.insert(combinedBuffer.end(), windowAckData.begin(), windowAckData.end());

            // Set Peer Bandwidth (type 6) - 2500000 bytes, dynamic limit type (2)
            auto peerBandwidthData = buildSetPeerBandwidth(2500000, 2);
            combinedBuffer.insert(combinedBuffer.end(), peerBandwidthData.begin(), peerBandwidthData.end());

            // Set Chunk Size (type 1) - 4096 bytes for better performance
            auto chunkSizeData = buildProtocolControlMessage(1, 4096);
            combinedBuffer.insert(combinedBuffer.end(), chunkSizeData.begin(), chunkSizeData.end());

            // Update our chunk size for future messages
            ctx->serverChunkSize = 4096;
        }

        // Add response messages (use the server chunk size that was set above)
        auto& responses = result.value().responses;
        for (const auto& response : responses) {
            // Use appropriate chunk stream ID based on response type:
            // - Chunk stream ID 3 for connect-level commands (_result to connect)
            // - Chunk stream ID 4 for stream-level commands (onStatus for publish/play)
            uint32_t responseChunkStreamId = chunk.chunkStreamId;
            if (response.messageStreamId != 0) {
                // Stream-level response (publish, play, etc.) - use chunk stream ID 4
                responseChunkStreamId = 4;
            }
            auto responseData = buildCommandResponse(response, responseChunkStreamId, ctx->serverChunkSize);
            combinedBuffer.insert(combinedBuffer.end(), responseData.begin(), responseData.end());
        }

        // Send all messages in one atomic write
        if (!combinedBuffer.empty()) {
            core::Buffer sendBuffer(combinedBuffer.data(), combinedBuffer.size());
            networkPal_->asyncWrite(ctx->socket, sendBuffer,
                [this, ctx](core::Result<size_t, pal::NetworkError> writeResult) {
                    if (writeResult.isError()) {
                        closeConnection(ctx, "Write error during command response");
                        return;
                    }
                    bytesSent_ += writeResult.value();
                }
            );
        }

        // Update connection state based on session context
        if (ctx->sessionContext.state == protocol::SessionState::Connected &&
            ctx->state == ConnectionContext::State::Handshaking) {
            ctx->state = ConnectionContext::State::Connected;
            emitEvent(ServerEventType::ClientConnected, "",
                     ctx->clientIP, ctx->clientPort, "Connected");
        } else if (ctx->sessionContext.state == protocol::SessionState::Publishing) {
            ctx->state = ConnectionContext::State::Publishing;
            ctx->streamKey = ctx->sessionContext.streamKey;
            activeStreams_++;
            totalStreams_++;
            emitEvent(ServerEventType::StreamStarted, ctx->streamKey,
                     ctx->clientIP, ctx->clientPort, "Publishing started");
        } else if (ctx->sessionContext.state == protocol::SessionState::Subscribing) {
            ctx->state = ConnectionContext::State::Subscribing;
            ctx->streamKey = ctx->sessionContext.streamKey;
            emitEvent(ServerEventType::SubscriberJoined, ctx->streamKey,
                     ctx->clientIP, ctx->clientPort, "Subscriber joined");

            // Send initial stream data to new subscriber
            sendInitialStreamData(ctx);
        }
    }

    // Build User Control Message (type 4)
    std::vector<uint8_t> buildUserControlMessage(uint16_t eventType, uint32_t eventData) {
        std::vector<uint8_t> payload(6);
        // Event type (2 bytes, big-endian)
        payload[0] = static_cast<uint8_t>((eventType >> 8) & 0xFF);
        payload[1] = static_cast<uint8_t>(eventType & 0xFF);
        // Event data (4 bytes, big-endian)
        payload[2] = static_cast<uint8_t>((eventData >> 24) & 0xFF);
        payload[3] = static_cast<uint8_t>((eventData >> 16) & 0xFF);
        payload[4] = static_cast<uint8_t>((eventData >> 8) & 0xFF);
        payload[5] = static_cast<uint8_t>(eventData & 0xFF);

        return serializeChunk(
            2,    // chunk stream ID 2 for protocol control
            0,    // timestamp
            6,    // message length (6 bytes)
            4,    // message type 4 = User Control Message
            0,    // message stream ID 0
            payload
        );
    }

    void sendInitialStreamData(std::shared_ptr<ConnectionContext> ctx) {
        if (ctx->streamKey.empty()) {
            return;
        }

        std::cerr << "[DEBUG] sendInitialStreamData: streamKey=" << ctx->streamKey << std::endl;

        // Build combined buffer for all initial messages
        std::vector<uint8_t> combinedBuffer;

        // 0. Send Set Chunk Size (type 1) - CRITICAL: Must be sent BEFORE any media data
        //    This tells the subscriber's chunk parser to expect 4096-byte chunks
        auto setChunkSizeMsg = buildProtocolControlMessage(1, 4096);
        combinedBuffer.insert(combinedBuffer.end(), setChunkSizeMsg.begin(), setChunkSizeMsg.end());
        std::cerr << "[DEBUG] Added Set Chunk Size (4096) message" << std::endl;

        // 1. Send User Control Message: Stream Begin (event type 0)
        //    This tells the client that stream ID 1 is about to start
        auto streamBeginData = buildUserControlMessage(0, 1);  // Stream Begin, stream ID 1
        combinedBuffer.insert(combinedBuffer.end(), streamBeginData.begin(), streamBeginData.end());

        std::cerr << "[DEBUG] Added StreamBegin message" << std::endl;

        // 2. Send stored stream data (metadata, sequence headers, and full GOP)
        {
            std::lock_guard<std::mutex> lock(streamDataMutex_);
            auto it = streamDataMap_.find(ctx->streamKey);
            if (it != streamDataMap_.end()) {
                StreamData& streamData = it->second;

                // Send metadata (type 18) if available
                if (streamData.hasMetadata && !streamData.metadata.empty()) {
                    auto metadataChunk = serializeChunk(
                        5,  // chunk stream ID 5 for data messages
                        0,  // timestamp
                        streamData.metadata.size(),
                        18,  // AMF0 Data message
                        1,   // message stream ID 1
                        streamData.metadata,
                        4096
                    );
                    combinedBuffer.insert(combinedBuffer.end(), metadataChunk.begin(), metadataChunk.end());
                    std::cerr << "[DEBUG] Added metadata, size=" << streamData.metadata.size() << std::endl;
                }

                // Send video sequence header (type 9) if available
                if (streamData.hasVideoHeader && !streamData.videoSequenceHeader.empty()) {
                    auto videoSeqChunk = serializeChunk(
                        6,  // chunk stream ID 6 for video
                        0,  // timestamp 0 for sequence header
                        streamData.videoSequenceHeader.size(),
                        9,   // Video message
                        1,   // message stream ID 1
                        streamData.videoSequenceHeader,
                        4096
                    );
                    combinedBuffer.insert(combinedBuffer.end(), videoSeqChunk.begin(), videoSeqChunk.end());
                    std::cerr << "[DEBUG] Added video sequence header, size=" << streamData.videoSequenceHeader.size() << std::endl;
                }

                // Send audio sequence header (type 8) if available
                if (streamData.hasAudioHeader && !streamData.audioSequenceHeader.empty()) {
                    auto audioSeqChunk = serializeChunk(
                        4,  // chunk stream ID 4 for audio
                        0,  // timestamp 0 for sequence header
                        streamData.audioSequenceHeader.size(),
                        8,   // Audio message
                        1,   // message stream ID 1
                        streamData.audioSequenceHeader,
                        4096
                    );
                    combinedBuffer.insert(combinedBuffer.end(), audioSeqChunk.begin(), audioSeqChunk.end());
                    std::cerr << "[DEBUG] Added audio sequence header, size=" << streamData.audioSequenceHeader.size() << std::endl;
                }

                // GOP CACHE INSTANT PLAYBACK (nginx-http-flv-module style)
                // Send cached frames starting from the last keyframe
                // This provides instant playback with clean H.264 decoder state
                auto gopFrames = streamData.gopBuffer.getFromLastKeyframe();
                if (!gopFrames.empty()) {
                    std::cerr << "[DEBUG] Sending GOP cache: " << gopFrames.size()
                              << " frames from last keyframe" << std::endl;

                    for (const auto& frame : gopFrames) {
                        uint32_t chunkStreamId = (frame.type == MediaType::Video) ? 6 : 4;
                        uint8_t messageTypeId = (frame.type == MediaType::Video) ? 9 : 8;

                        auto frameChunk = serializeChunk(
                            chunkStreamId,
                            frame.timestamp,
                            frame.data.size(),
                            messageTypeId,
                            1,  // message stream ID 1
                            frame.data,
                            4096
                        );
                        combinedBuffer.insert(combinedBuffer.end(), frameChunk.begin(), frameChunk.end());
                    }

                    std::cerr << "[DEBUG] Added " << gopFrames.size()
                              << " GOP frames, total buffer size=" << combinedBuffer.size() << std::endl;

                    // Since we sent GOP from keyframe, subscriber doesn't need to wait
                    ctx->waitingForKeyframe = false;
                    ctx->receivedFirstKeyframe = true;
                } else {
                    std::cerr << "[DEBUG] No GOP cache available, will wait for next keyframe" << std::endl;
                }

            } else {
                std::cerr << "[DEBUG] No stream data found for key: " << ctx->streamKey << std::endl;
            }
        }

        // Send initial data (metadata + sequence headers only)
        if (!combinedBuffer.empty()) {
            std::cerr << "[DEBUG] Sending initial stream data (metadata + seq headers), size=" << combinedBuffer.size() << std::endl;

            // Mark initial data as sent - subscriber will wait for keyframe
            ctx->initialDataSent = true;

            core::Buffer sendBuffer(combinedBuffer.data(), combinedBuffer.size());
            networkPal_->asyncWrite(ctx->socket, sendBuffer,
                [this, ctx](core::Result<size_t, pal::NetworkError> writeResult) {
                    if (writeResult.isError()) {
                        std::cerr << "[DEBUG] Failed to send initial stream data: " << writeResult.error().message << std::endl;
                        ctx->initialDataSent = false;
                        closeConnection(ctx, "Write error during initial stream data");
                        return;
                    }
                    bytesSent_ += writeResult.value();
                    std::cerr << "[DEBUG] Successfully sent initial stream data, waiting for keyframe" << std::endl;
                }
            );
        } else {
            // No initial data yet, but mark as ready
            ctx->initialDataSent = true;
            std::cerr << "[DEBUG] No initial data available yet, will relay when keyframe arrives" << std::endl;
        }
    }

    void sendCommandResponse(std::shared_ptr<ConnectionContext> ctx,
                            const protocol::CommandResponseMessage& response,
                            uint32_t chunkStreamId) {
        // Encode response to AMF0
        std::vector<uint8_t> payload;

        // Encode command name
        auto nameResult = amfCodec_->encodeAMF0(protocol::AMFValue::makeString(response.commandName));
        if (nameResult.isSuccess()) {
            auto& nameBytes = nameResult.value();
            payload.insert(payload.end(), nameBytes.begin(), nameBytes.end());
        }

        // Encode transaction ID
        auto txIdResult = amfCodec_->encodeAMF0(protocol::AMFValue::makeNumber(response.transactionId));
        if (txIdResult.isSuccess()) {
            auto& txIdBytes = txIdResult.value();
            payload.insert(payload.end(), txIdBytes.begin(), txIdBytes.end());
        }

        // Encode command object
        auto cmdObjResult = amfCodec_->encodeAMF0(response.commandObject);
        if (cmdObjResult.isSuccess()) {
            auto& cmdObjBytes = cmdObjResult.value();
            payload.insert(payload.end(), cmdObjBytes.begin(), cmdObjBytes.end());
        }

        // Encode additional arguments
        for (const auto& arg : response.args) {
            auto argResult = amfCodec_->encodeAMF0(arg);
            if (argResult.isSuccess()) {
                auto& argBytes = argResult.value();
                payload.insert(payload.end(), argBytes.begin(), argBytes.end());
            }
        }

        // Build RTMP chunk(s) and send
        // Use message stream ID from response (0 for connect response)
        uint32_t messageStreamId = response.messageStreamId;

        // Serialize to RTMP chunk format using the server's announced chunk size
        std::vector<uint8_t> chunkData = serializeChunk(
            chunkStreamId,       // chunk stream ID (typically 3 for commands)
            0,                   // timestamp (0 for responses)
            payload.size(),      // message length
            20,                  // message type ID (AMF0 command)
            messageStreamId,     // message stream ID
            payload,             // payload
            ctx->serverChunkSize // use the server's announced chunk size
        );

        // Send the chunk
        core::Buffer sendBuffer(chunkData.data(), chunkData.size());
        networkPal_->asyncWrite(ctx->socket, sendBuffer,
            [this, ctx](core::Result<size_t, pal::NetworkError> writeResult) {
                if (writeResult.isError()) {
                    closeConnection(ctx, "Write error during command response");
                    return;
                }
                bytesSent_ += writeResult.value();
            }
        );
    }

    std::vector<uint8_t> serializeChunk(uint32_t csid, uint32_t timestamp,
                                        size_t messageLength, uint8_t typeId,
                                        uint32_t streamId,
                                        const std::vector<uint8_t>& payload,
                                        size_t chunkSize = 128) {
        std::vector<uint8_t> result;
        // Reserve space for: initial header (up to 15 bytes with extended ts) + payload +
        // continuation headers (up to 7 bytes each with extended ts)
        size_t continuationCount = payload.size() > 0 ? (payload.size() - 1) / chunkSize : 0;
        size_t extTsBytes = (timestamp >= 0xFFFFFF) ? 4 : 0;
        result.reserve(15 + payload.size() + continuationCount * (3 + extTsBytes));

        // Basic header (1-3 bytes depending on csid)
        // Format 0 (full header) for initial message
        uint8_t fmt = 0;

        if (csid < 64) {
            // 1 byte basic header
            result.push_back(static_cast<uint8_t>((fmt << 6) | csid));
        } else if (csid < 320) {
            // 2 byte basic header
            result.push_back(static_cast<uint8_t>((fmt << 6) | 0));
            result.push_back(static_cast<uint8_t>(csid - 64));
        } else {
            // 3 byte basic header
            result.push_back(static_cast<uint8_t>((fmt << 6) | 1));
            uint16_t csidMinus64 = static_cast<uint16_t>(csid - 64);
            result.push_back(static_cast<uint8_t>(csidMinus64 & 0xFF));
            result.push_back(static_cast<uint8_t>((csidMinus64 >> 8) & 0xFF));
        }

        // Message header (11 bytes for format 0)
        // Timestamp (3 bytes, big-endian)
        uint32_t ts = (timestamp >= 0xFFFFFF) ? 0xFFFFFF : timestamp;
        result.push_back(static_cast<uint8_t>((ts >> 16) & 0xFF));
        result.push_back(static_cast<uint8_t>((ts >> 8) & 0xFF));
        result.push_back(static_cast<uint8_t>(ts & 0xFF));

        // Message length (3 bytes, big-endian)
        result.push_back(static_cast<uint8_t>((messageLength >> 16) & 0xFF));
        result.push_back(static_cast<uint8_t>((messageLength >> 8) & 0xFF));
        result.push_back(static_cast<uint8_t>(messageLength & 0xFF));

        // Message type ID (1 byte)
        result.push_back(typeId);

        // Message stream ID (4 bytes, little-endian)
        result.push_back(static_cast<uint8_t>(streamId & 0xFF));
        result.push_back(static_cast<uint8_t>((streamId >> 8) & 0xFF));
        result.push_back(static_cast<uint8_t>((streamId >> 16) & 0xFF));
        result.push_back(static_cast<uint8_t>((streamId >> 24) & 0xFF));

        // Extended timestamp if needed
        if (timestamp >= 0xFFFFFF) {
            result.push_back(static_cast<uint8_t>((timestamp >> 24) & 0xFF));
            result.push_back(static_cast<uint8_t>((timestamp >> 16) & 0xFF));
            result.push_back(static_cast<uint8_t>((timestamp >> 8) & 0xFF));
            result.push_back(static_cast<uint8_t>(timestamp & 0xFF));
        }

        // Payload - split into chunks based on the specified chunk size
        size_t offset = 0;

        while (offset < payload.size()) {
            size_t bytesToWrite = std::min(chunkSize, payload.size() - offset);

            if (offset > 0) {
                // Continuation chunk - format 3 header (just basic header)
                if (csid < 64) {
                    result.push_back(static_cast<uint8_t>((3 << 6) | csid));
                } else if (csid < 320) {
                    result.push_back(static_cast<uint8_t>((3 << 6) | 0));
                    result.push_back(static_cast<uint8_t>(csid - 64));
                } else {
                    result.push_back(static_cast<uint8_t>((3 << 6) | 1));
                    uint16_t csidMinus64 = static_cast<uint16_t>(csid - 64);
                    result.push_back(static_cast<uint8_t>(csidMinus64 & 0xFF));
                    result.push_back(static_cast<uint8_t>((csidMinus64 >> 8) & 0xFF));
                }

                // Extended timestamp must be included in EVERY continuation chunk
                // when the original message had an extended timestamp (timestamp >= 0xFFFFFF)
                if (timestamp >= 0xFFFFFF) {
                    result.push_back(static_cast<uint8_t>((timestamp >> 24) & 0xFF));
                    result.push_back(static_cast<uint8_t>((timestamp >> 16) & 0xFF));
                    result.push_back(static_cast<uint8_t>((timestamp >> 8) & 0xFF));
                    result.push_back(static_cast<uint8_t>(timestamp & 0xFF));
                }
            }

            auto startIt = payload.begin() + static_cast<ptrdiff_t>(offset);
            auto endIt = payload.begin() + static_cast<ptrdiff_t>(offset + bytesToWrite);
            result.insert(result.end(), startIt, endIt);
            offset += bytesToWrite;
        }

        return result;
    }

    // Build a protocol control message (type 1, 3, 5 have 4-byte payload)
    // Type 1 = Set Chunk Size, Type 3 = Acknowledgement, Type 5 = Window Acknowledgement Size
    std::vector<uint8_t> buildProtocolControlMessage(uint8_t messageTypeId, uint32_t value) {
        std::vector<uint8_t> payload(4);
        payload[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
        payload[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
        payload[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
        payload[3] = static_cast<uint8_t>(value & 0xFF);

        return serializeChunk(
            2,              // chunk stream ID 2 for protocol control
            0,              // timestamp
            4,              // message length (4 bytes)
            messageTypeId,  // message type
            0,              // message stream ID 0
            payload
        );
    }

    // Build Set Peer Bandwidth message (type 6) - has 5 bytes payload (4 bytes value + 1 byte limit type)
    std::vector<uint8_t> buildSetPeerBandwidth(uint32_t windowSize, uint8_t limitType) {
        std::vector<uint8_t> payload(5);
        payload[0] = static_cast<uint8_t>((windowSize >> 24) & 0xFF);
        payload[1] = static_cast<uint8_t>((windowSize >> 16) & 0xFF);
        payload[2] = static_cast<uint8_t>((windowSize >> 8) & 0xFF);
        payload[3] = static_cast<uint8_t>(windowSize & 0xFF);
        payload[4] = limitType;  // 0=Hard, 1=Soft, 2=Dynamic

        return serializeChunk(
            2,    // chunk stream ID 2 for protocol control
            0,    // timestamp
            5,    // message length (5 bytes)
            6,    // message type 6 = Set Peer Bandwidth
            0,    // message stream ID 0
            payload
        );
    }

    // Build a command response message
    std::vector<uint8_t> buildCommandResponse(const protocol::CommandResponseMessage& response,
                                               uint32_t chunkStreamId,
                                               size_t chunkSize = 128) {
        // Encode response to AMF0
        std::vector<uint8_t> payload;

        // Encode command name
        auto nameResult = amfCodec_->encodeAMF0(protocol::AMFValue::makeString(response.commandName));
        if (nameResult.isSuccess()) {
            auto& nameBytes = nameResult.value();
            payload.insert(payload.end(), nameBytes.begin(), nameBytes.end());
        }

        // Encode transaction ID
        auto txIdResult = amfCodec_->encodeAMF0(protocol::AMFValue::makeNumber(response.transactionId));
        if (txIdResult.isSuccess()) {
            auto& txIdBytes = txIdResult.value();
            payload.insert(payload.end(), txIdBytes.begin(), txIdBytes.end());
        }

        // Encode command object
        auto cmdObjResult = amfCodec_->encodeAMF0(response.commandObject);
        if (cmdObjResult.isSuccess()) {
            auto& cmdObjBytes = cmdObjResult.value();
            payload.insert(payload.end(), cmdObjBytes.begin(), cmdObjBytes.end());
        }

        // Encode additional arguments
        for (const auto& arg : response.args) {
            auto argResult = amfCodec_->encodeAMF0(arg);
            if (argResult.isSuccess()) {
                auto& argBytes = argResult.value();
                payload.insert(payload.end(), argBytes.begin(), argBytes.end());
            }
        }

        // Use message stream ID from response (0 for connect response)
        uint32_t messageStreamId = response.messageStreamId;

        // Serialize to RTMP chunk format using the specified chunk size
        return serializeChunk(
            chunkStreamId,       // chunk stream ID (typically 3 for commands)
            0,                   // timestamp (0 for responses)
            payload.size(),      // message length
            20,                  // message type ID (AMF0 command)
            messageStreamId,     // message stream ID
            payload,             // payload
            chunkSize            // use the specified chunk size
        );
    }

    // Send a protocol control message (type 1, 3, 5 have 4-byte payload)
    // Type 1 = Set Chunk Size, Type 3 = Acknowledgement, Type 5 = Window Acknowledgement Size
    void sendProtocolControlMessage(std::shared_ptr<ConnectionContext> ctx,
                                    uint8_t messageTypeId, uint32_t value) {
        // Protocol control messages use chunk stream ID 2 and message stream ID 0
        std::vector<uint8_t> payload(4);
        payload[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
        payload[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
        payload[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
        payload[3] = static_cast<uint8_t>(value & 0xFF);

        std::vector<uint8_t> chunkData = serializeChunk(
            2,              // chunk stream ID 2 for protocol control
            0,              // timestamp
            4,              // message length (4 bytes)
            messageTypeId,  // message type
            0,              // message stream ID 0
            payload
        );

        core::Buffer sendBuffer(chunkData.data(), chunkData.size());
        networkPal_->asyncWrite(ctx->socket, sendBuffer,
            [this, ctx](core::Result<size_t, pal::NetworkError> writeResult) {
                if (writeResult.isError()) {
                    closeConnection(ctx, "Write error during protocol control message");
                    return;
                }
                bytesSent_ += writeResult.value();
            }
        );
    }

    // Send Set Peer Bandwidth (type 6) - has 5 bytes payload (4 bytes value + 1 byte limit type)
    void sendSetPeerBandwidth(std::shared_ptr<ConnectionContext> ctx,
                              uint32_t windowSize, uint8_t limitType) {
        std::vector<uint8_t> payload(5);
        payload[0] = static_cast<uint8_t>((windowSize >> 24) & 0xFF);
        payload[1] = static_cast<uint8_t>((windowSize >> 16) & 0xFF);
        payload[2] = static_cast<uint8_t>((windowSize >> 8) & 0xFF);
        payload[3] = static_cast<uint8_t>(windowSize & 0xFF);
        payload[4] = limitType;  // 0=Hard, 1=Soft, 2=Dynamic

        std::vector<uint8_t> chunkData = serializeChunk(
            2,    // chunk stream ID 2 for protocol control
            0,    // timestamp
            5,    // message length (5 bytes)
            6,    // message type 6 = Set Peer Bandwidth
            0,    // message stream ID 0
            payload
        );

        core::Buffer sendBuffer(chunkData.data(), chunkData.size());
        networkPal_->asyncWrite(ctx->socket, sendBuffer,
            [this, ctx](core::Result<size_t, pal::NetworkError> writeResult) {
                if (writeResult.isError()) {
                    closeConnection(ctx, "Write error during set peer bandwidth");
                    return;
                }
                bytesSent_ += writeResult.value();
            }
        );
    }

    void closeConnection(std::shared_ptr<ConnectionContext> ctx, const std::string& reason) {
        // Handle stream cleanup if publishing/subscribing
        if (ctx->state == ConnectionContext::State::Publishing) {
            if (streamRegistry_ && !ctx->streamKey.empty()) {
                core::StreamKey key("live", ctx->streamKey);
                streamRegistry_->unregisterStream(key);
            }
            activeStreams_--;
            emitEvent(ServerEventType::StreamEnded, ctx->streamKey,
                     ctx->clientIP, ctx->clientPort, "Stream ended");
        } else if (ctx->state == ConnectionContext::State::Subscribing) {
            if (streamRegistry_ && !ctx->streamKey.empty()) {
                core::StreamKey key("live", ctx->streamKey);
                streamRegistry_->removeSubscriber(key, ctx->id);
            }
            emitEvent(ServerEventType::SubscriberLeft, ctx->streamKey,
                     ctx->clientIP, ctx->clientPort, "Subscriber left");
        }

        // Close socket
        networkPal_->closeSocket(ctx->socket);

        // Remove from connections map
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            connections_.erase(ctx->id);
        }

        activeConnections_--;

        emitEvent(ServerEventType::ClientDisconnected, ctx->streamKey,
                 ctx->clientIP, ctx->clientPort, reason);
    }

    void closeAllConnections() {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        for (auto& pair : connections_) {
            networkPal_->closeSocket(pair.second->socket);
        }
        connections_.clear();
        activeConnections_ = 0;
        activeStreams_ = 0;
    }
#endif  // __APPLE__

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

#if defined(__APPLE__)
    // Network
    std::unique_ptr<PlatformNetworkPAL> networkPal_;
    pal::ServerSocket serverSocket_;
    std::thread eventLoopThread_;

    // Connections
    std::mutex connectionsMutex_;
    std::unordered_map<uint64_t, std::shared_ptr<ConnectionContext>> connections_;
#endif

    std::atomic<uint64_t> nextConnectionId_;

    // Services
    std::shared_ptr<streaming::StreamRegistry> streamRegistry_;
    std::shared_ptr<protocol::IAMFCodec> amfCodec_;
    std::unique_ptr<core::ConnectionPool> connectionPool_;
    std::unique_ptr<core::ShutdownCoordinator> shutdownCoordinator_;
    std::unique_ptr<core::ErrorIsolation> errorIsolation_;
    std::unique_ptr<core::AuthService> authService_;

    // Stream data storage (for media relay)
    std::mutex streamDataMutex_;
    std::unordered_map<std::string, StreamData> streamDataMap_;  // streamKey -> StreamData

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
