// OpenRTMP - Cross-platform RTMP Server
// Error Isolation and Recovery Implementation
//
// Thread-safe implementation with:
// - Connection error isolation
// - Memory pressure detection
// - Component crash recovery
// - Circuit breaker pattern
// - Diagnostic logging
// - Health check API
//
// Requirements coverage:
// - Requirement 20.1: Single connection failure isolation
// - Requirement 20.2: Memory allocation failure handling
// - Requirement 20.3: Thread/task crash recovery
// - Requirement 20.4: Circuit breaker for external service failures
// - Requirement 20.5: Diagnostic logging before unrecoverable error termination
// - Requirement 20.6: Health check API for external monitoring

#include "openrtmp/core/error_isolation.hpp"

#include <sstream>
#include <iomanip>
#include <algorithm>

namespace openrtmp {
namespace core {

// =============================================================================
// Constructor / Destructor
// =============================================================================

ErrorIsolation::ErrorIsolation()
    : config_()
{
}

ErrorIsolation::ErrorIsolation(const ErrorIsolationConfig& config)
    : config_(config)
{
    // Validate and cap memory threshold
    if (config_.memoryThresholdPercent > 100) {
        config_.memoryThresholdPercent = 100;
    }
}

ErrorIsolation::~ErrorIsolation() {
    shuttingDown_ = true;

    // Wait for any restart threads to complete
    std::unique_lock<std::shared_mutex> lock(componentsMutex_);
    for (auto& [name, info] : components_) {
        if (info && info->restartThread.joinable()) {
            lock.unlock();
            info->restartThread.join();
            lock.lock();
        }
    }
}

ErrorIsolation::ErrorIsolation(ErrorIsolation&& other) noexcept
    : config_(std::move(other.config_))
    , connections_(std::move(other.connections_))
    , memoryUsedBytes_(other.memoryUsedBytes_.load())
    , memoryTotalBytes_(other.memoryTotalBytes_.load())
    , underMemoryPressure_(other.underMemoryPressure_.load())
    , components_(std::move(other.components_))
    , circuitBreakers_(std::move(other.circuitBreakers_))
    , stats_(std::move(other.stats_))
    , connectionErrorCallback_(std::move(other.connectionErrorCallback_))
    , memoryPressureCallback_(std::move(other.memoryPressureCallback_))
    , diagnosticLoggingCallback_(std::move(other.diagnosticLoggingCallback_))
{
}

ErrorIsolation& ErrorIsolation::operator=(ErrorIsolation&& other) noexcept {
    if (this != &other) {
        std::unique_lock<std::shared_mutex> configLock(configMutex_);
        std::unique_lock<std::shared_mutex> connectionsLock(connectionsMutex_);
        std::unique_lock<std::shared_mutex> componentsLock(componentsMutex_);
        std::unique_lock<std::shared_mutex> circuitBreakersLock(circuitBreakersMutex_);
        std::unique_lock<std::mutex> statsLock(statsMutex_);
        std::unique_lock<std::mutex> callbackLock(callbackMutex_);

        config_ = std::move(other.config_);
        connections_ = std::move(other.connections_);
        memoryUsedBytes_ = other.memoryUsedBytes_.load();
        memoryTotalBytes_ = other.memoryTotalBytes_.load();
        underMemoryPressure_ = other.underMemoryPressure_.load();
        components_ = std::move(other.components_);
        circuitBreakers_ = std::move(other.circuitBreakers_);
        stats_ = std::move(other.stats_);
        connectionErrorCallback_ = std::move(other.connectionErrorCallback_);
        memoryPressureCallback_ = std::move(other.memoryPressureCallback_);
        diagnosticLoggingCallback_ = std::move(other.diagnosticLoggingCallback_);
    }
    return *this;
}

// =============================================================================
// Connection Error Isolation (Requirement 20.1)
// =============================================================================

void ErrorIsolation::registerConnection(ConnectionId connectionId) {
    std::unique_lock<std::shared_mutex> lock(connectionsMutex_);

    ConnectionInfo info;
    info.id = connectionId;
    info.failed = false;
    connections_[connectionId] = info;
}

void ErrorIsolation::unregisterConnection(ConnectionId connectionId) {
    std::unique_lock<std::shared_mutex> lock(connectionsMutex_);
    connections_.erase(connectionId);
}

void ErrorIsolation::handleConnectionError(
    ConnectionId connectionId,
    ErrorCode code,
    const std::string& message
) {
    // Update connection state
    {
        std::unique_lock<std::shared_mutex> lock(connectionsMutex_);
        auto it = connections_.find(connectionId);
        if (it != connections_.end()) {
            it->second.failed = true;
            it->second.lastError = message;
        }
    }

    // Update statistics
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.connectionFailures++;
    }

    // Invoke callback
    ConnectionErrorCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = connectionErrorCallback_;
    }

    safeInvokeCallback(callback, connectionId, code, message);
}

bool ErrorIsolation::isConnectionFailed(ConnectionId connectionId) const {
    std::shared_lock<std::shared_mutex> lock(connectionsMutex_);
    auto it = connections_.find(connectionId);
    if (it != connections_.end()) {
        return it->second.failed;
    }
    return false;
}

void ErrorIsolation::cleanupConnection(ConnectionId connectionId) {
    std::unique_lock<std::shared_mutex> lock(connectionsMutex_);
    connections_.erase(connectionId);
}

size_t ErrorIsolation::connectionCount() const {
    std::shared_lock<std::shared_mutex> lock(connectionsMutex_);
    return connections_.size();
}

size_t ErrorIsolation::activeConnectionCount() const {
    std::shared_lock<std::shared_mutex> lock(connectionsMutex_);
    size_t count = 0;
    for (const auto& [id, info] : connections_) {
        if (!info.failed) {
            count++;
        }
    }
    return count;
}

void ErrorIsolation::setConnectionErrorCallback(ConnectionErrorCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    connectionErrorCallback_ = std::move(callback);
}

// =============================================================================
// Memory Pressure Handling (Requirement 20.2)
// =============================================================================

void ErrorIsolation::reportMemoryUsage(uint64_t usedBytes, uint64_t totalBytes) {
    memoryUsedBytes_ = usedBytes;
    memoryTotalBytes_ = totalBytes;

    uint32_t threshold;
    {
        std::shared_lock<std::shared_mutex> lock(configMutex_);
        threshold = config_.memoryThresholdPercent;
    }

    uint32_t usagePercent = calculateMemoryPercent();
    bool wasPressure = underMemoryPressure_.load();
    bool isPressure = usagePercent >= threshold;
    underMemoryPressure_ = isPressure;

    // Invoke callback only when transitioning to pressure state
    if (isPressure && !wasPressure) {
        MemoryPressureCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            callback = memoryPressureCallback_;
        }
        safeInvokeCallback(callback, usedBytes, totalBytes);
    }
}

void ErrorIsolation::recordMemoryAllocationFailure(
    const std::string& /* component */,
    uint64_t /* requestedBytes */
) {
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.memoryAllocationFailures++;
}

bool ErrorIsolation::isUnderMemoryPressure() const {
    return underMemoryPressure_.load();
}

bool ErrorIsolation::canAcceptNewConnection() const {
    return !underMemoryPressure_.load();
}

void ErrorIsolation::setMemoryPressureCallback(MemoryPressureCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    memoryPressureCallback_ = std::move(callback);
}

// =============================================================================
// Component Restart (Requirement 20.3)
// =============================================================================

void ErrorIsolation::registerComponent(
    const std::string& name,
    ComponentRestartFunction restartFunc
) {
    std::unique_lock<std::shared_mutex> lock(componentsMutex_);

    auto info = std::make_unique<ComponentInfo>();
    info->name = name;
    info->state = ComponentState::Running;
    info->restartFunc = std::move(restartFunc);
    info->restartAttempts = 0;
    components_[name] = std::move(info);
}

void ErrorIsolation::unregisterComponent(const std::string& name) {
    std::unique_lock<std::shared_mutex> lock(componentsMutex_);
    auto it = components_.find(name);
    if (it != components_.end()) {
        if (it->second && it->second->restartThread.joinable()) {
            lock.unlock();
            it->second->restartThread.join();
            lock.lock();
        }
        components_.erase(it);
    }
}

void ErrorIsolation::reportComponentCrash(
    const std::string& name,
    const std::string& /* errorDetails */
) {
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.componentCrashes++;
    }

    // Check if component is registered
    {
        std::shared_lock<std::shared_mutex> lock(componentsMutex_);
        if (components_.find(name) == components_.end()) {
            return;
        }
    }

    // Attempt restart in a separate thread
    attemptComponentRestart(name);
}

ComponentState ErrorIsolation::getComponentState(const std::string& name) const {
    std::shared_lock<std::shared_mutex> lock(componentsMutex_);
    auto it = components_.find(name);
    if (it != components_.end() && it->second) {
        return it->second->state;
    }
    return ComponentState::Failed;  // Unknown component treated as failed
}

void ErrorIsolation::attemptComponentRestart(const std::string& name) {
    std::unique_lock<std::shared_mutex> lock(componentsMutex_);
    auto it = components_.find(name);
    if (it == components_.end() || !it->second) {
        return;
    }

    auto* info = it->second.get();

    // Wait for any existing restart thread
    if (info->restartThread.joinable()) {
        lock.unlock();
        info->restartThread.join();
        lock.lock();
        it = components_.find(name);
        if (it == components_.end() || !it->second) {
            return;
        }
        info = it->second.get();
    }

    info->state = ComponentState::Restarting;

    // Get config values
    uint32_t maxAttempts;
    uint32_t delayMs;
    {
        std::shared_lock<std::shared_mutex> configLock(configMutex_);
        maxAttempts = config_.componentRestartMaxAttempts;
        delayMs = config_.componentRestartDelayMs;
    }

    // Capture restart function
    auto restartFunc = info->restartFunc;

    info->restartThread = std::thread([this, name, restartFunc, maxAttempts, delayMs]() {
        uint32_t attempts = 0;
        bool success = false;

        while (attempts < maxAttempts && !shuttingDown_.load()) {
            attempts++;

            // Small delay before restart
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

            if (shuttingDown_.load()) {
                break;
            }

            try {
                if (restartFunc && restartFunc()) {
                    success = true;
                    {
                        std::lock_guard<std::mutex> lock(statsMutex_);
                        stats_.componentRestarts++;
                    }
                    break;
                }
            } catch (...) {
                // Restart threw exception, try again
            }
        }

        // Update component state
        std::unique_lock<std::shared_mutex> lock(componentsMutex_);
        auto it = components_.find(name);
        if (it != components_.end() && it->second) {
            it->second->restartAttempts = attempts;
            it->second->state = success ? ComponentState::Running : ComponentState::Failed;
        }
    });
}

// =============================================================================
// Circuit Breaker (Requirement 20.4)
// =============================================================================

void ErrorIsolation::registerExternalService(const std::string& serviceName) {
    std::unique_lock<std::shared_mutex> lock(circuitBreakersMutex_);

    CircuitBreakerInfo info;
    info.serviceName = serviceName;
    info.state = CircuitBreakerState::Closed;
    info.failureCount = 0;
    circuitBreakers_[serviceName] = info;
}

void ErrorIsolation::unregisterExternalService(const std::string& serviceName) {
    std::unique_lock<std::shared_mutex> lock(circuitBreakersMutex_);
    circuitBreakers_.erase(serviceName);
}

void ErrorIsolation::recordExternalServiceFailure(const std::string& serviceName) {
    std::unique_lock<std::shared_mutex> lock(circuitBreakersMutex_);
    auto it = circuitBreakers_.find(serviceName);
    if (it == circuitBreakers_.end()) {
        return;
    }

    auto& info = it->second;

    if (info.state == CircuitBreakerState::HalfOpen) {
        // Failure in half-open state, reopen circuit
        info.state = CircuitBreakerState::Open;
        info.lastFailureTime = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> statsLock(statsMutex_);
            stats_.circuitBreakerTrips++;
        }
    } else if (info.state == CircuitBreakerState::Closed) {
        info.failureCount++;

        uint32_t threshold;
        {
            std::shared_lock<std::shared_mutex> configLock(configMutex_);
            threshold = config_.circuitBreakerThreshold;
        }

        if (info.failureCount >= threshold) {
            info.state = CircuitBreakerState::Open;
            info.lastFailureTime = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> statsLock(statsMutex_);
                stats_.circuitBreakerTrips++;
            }
        }
    }
}

void ErrorIsolation::recordExternalServiceSuccess(const std::string& serviceName) {
    std::unique_lock<std::shared_mutex> lock(circuitBreakersMutex_);
    auto it = circuitBreakers_.find(serviceName);
    if (it == circuitBreakers_.end()) {
        return;
    }

    auto& info = it->second;

    if (info.state == CircuitBreakerState::HalfOpen) {
        // Success in half-open state, close circuit
        info.state = CircuitBreakerState::Closed;
        info.failureCount = 0;
    } else if (info.state == CircuitBreakerState::Closed) {
        // Reset failure count on success
        info.failureCount = 0;
    }
}

bool ErrorIsolation::isExternalServiceAvailable(const std::string& serviceName) {
    std::unique_lock<std::shared_mutex> lock(circuitBreakersMutex_);
    auto it = circuitBreakers_.find(serviceName);
    if (it == circuitBreakers_.end()) {
        return true;  // Unknown service assumed available
    }

    auto& info = it->second;

    if (info.state == CircuitBreakerState::Closed) {
        return true;
    }

    if (info.state == CircuitBreakerState::Open) {
        // Check if we should transition to half-open
        uint32_t resetMs;
        {
            std::shared_lock<std::shared_mutex> configLock(configMutex_);
            resetMs = config_.circuitBreakerResetMs;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - info.lastFailureTime
        );

        if (elapsed.count() >= resetMs) {
            info.state = CircuitBreakerState::HalfOpen;
            return true;
        }

        return false;
    }

    // Half-open: allow one request to test
    return true;
}

CircuitBreakerState ErrorIsolation::getCircuitBreakerState(
    const std::string& serviceName
) const {
    std::shared_lock<std::shared_mutex> lock(circuitBreakersMutex_);
    auto it = circuitBreakers_.find(serviceName);
    if (it != circuitBreakers_.end()) {
        return it->second.state;
    }
    return CircuitBreakerState::Closed;  // Unknown service assumed closed
}

// =============================================================================
// Diagnostic Logging (Requirement 20.5)
// =============================================================================

void ErrorIsolation::reportUnrecoverableError(
    const std::string& /* errorMessage */,
    const std::string& /* errorDetails */
) {
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.unrecoverableErrors++;
    }

    // Build diagnostics
    std::string diagnostics = buildDiagnostics();

    // Invoke callback
    DiagnosticLoggingCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = diagnosticLoggingCallback_;
    }

    safeInvokeCallback(callback, diagnostics);
}

std::string ErrorIsolation::getDiagnosticDump() const {
    return buildDiagnostics();
}

void ErrorIsolation::setDiagnosticLoggingCallback(DiagnosticLoggingCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    diagnosticLoggingCallback_ = std::move(callback);
}

// =============================================================================
// Health Check API (Requirement 20.6)
// =============================================================================

HealthStatus ErrorIsolation::getHealthStatus() const {
    HealthStatus status;

    // Memory info
    status.memoryUsedBytes = memoryUsedBytes_.load();
    status.memoryTotalBytes = memoryTotalBytes_.load();

    // Connection count
    {
        std::shared_lock<std::shared_mutex> lock(connectionsMutex_);
        status.activeConnections = static_cast<uint32_t>(connections_.size());
    }

    // Component statuses
    {
        std::shared_lock<std::shared_mutex> lock(componentsMutex_);
        for (const auto& [name, info] : components_) {
            if (info) {
                ComponentHealthStatus compStatus;
                compStatus.name = name;
                compStatus.state = info->state;
                compStatus.restartAttempts = info->restartAttempts;
                status.componentStatuses.push_back(compStatus);
            }
        }
    }

    // Circuit breaker statuses
    {
        std::shared_lock<std::shared_mutex> lock(circuitBreakersMutex_);
        for (const auto& [name, info] : circuitBreakers_) {
            CircuitBreakerStatus cbStatus;
            cbStatus.serviceName = name;
            cbStatus.state = info.state;
            cbStatus.failureCount = info.failureCount;
            status.circuitBreakerStatuses.push_back(cbStatus);
        }
    }

    // Determine overall health state
    status.state = HealthState::Healthy;
    status.details = "OK";

    // Check for failed components
    for (const auto& comp : status.componentStatuses) {
        if (comp.state == ComponentState::Failed) {
            status.state = HealthState::Unhealthy;
            status.details = "Component failed: " + comp.name;
            break;
        }
    }

    // Check for memory pressure (degrades to Degraded if not already Unhealthy)
    if (status.state != HealthState::Unhealthy && underMemoryPressure_.load()) {
        status.state = HealthState::Degraded;
        status.details = "System under memory pressure";
    }

    // Check for open circuit breakers
    if (status.state == HealthState::Healthy) {
        for (const auto& cb : status.circuitBreakerStatuses) {
            if (cb.state == CircuitBreakerState::Open) {
                status.state = HealthState::Degraded;
                status.details = "External service unavailable: " + cb.serviceName;
                break;
            }
        }
    }

    return status;
}

// =============================================================================
// Statistics
// =============================================================================

ErrorIsolationStatistics ErrorIsolation::getStatistics() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void ErrorIsolation::resetStatistics() {
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_ = ErrorIsolationStatistics();
}

// =============================================================================
// Configuration
// =============================================================================

void ErrorIsolation::updateConfig(std::function<void(ErrorIsolationConfig&)> updater) {
    std::unique_lock<std::shared_mutex> lock(configMutex_);
    updater(config_);

    // Validate
    if (config_.memoryThresholdPercent > 100) {
        config_.memoryThresholdPercent = 100;
    }
}

ErrorIsolationConfig ErrorIsolation::getConfig() const {
    std::shared_lock<std::shared_mutex> lock(configMutex_);
    return config_;
}

// =============================================================================
// Private Methods
// =============================================================================

template<typename Callback, typename... Args>
void ErrorIsolation::safeInvokeCallback(const Callback& callback, Args&&... args) {
    if (callback) {
        try {
            callback(std::forward<Args>(args)...);
        } catch (...) {
            // Swallow callback exceptions to prevent cascading failures
        }
    }
}

uint32_t ErrorIsolation::calculateMemoryPercent() const {
    uint64_t total = memoryTotalBytes_.load();
    if (total == 0) {
        return 0;
    }
    uint64_t used = memoryUsedBytes_.load();
    return static_cast<uint32_t>((used * 100) / total);
}

std::string ErrorIsolation::buildDiagnostics() const {
    std::ostringstream oss;
    oss << "=== OpenRTMP Diagnostic Dump ===" << std::endl;

    // Memory info
    oss << std::endl << "--- Memory ---" << std::endl;
    oss << "Used: " << memoryUsedBytes_.load() << " bytes" << std::endl;
    oss << "Total: " << memoryTotalBytes_.load() << " bytes" << std::endl;
    oss << "Under pressure: " << (underMemoryPressure_.load() ? "Yes" : "No") << std::endl;

    // Connection info
    oss << std::endl << "--- Connections ---" << std::endl;
    {
        std::shared_lock<std::shared_mutex> lock(connectionsMutex_);
        oss << "Total: " << connections_.size() << std::endl;
        size_t failed = 0;
        for (const auto& [id, info] : connections_) {
            if (info.failed) {
                failed++;
            }
        }
        oss << "Failed: " << failed << std::endl;
    }

    // Component info
    oss << std::endl << "--- Components ---" << std::endl;
    {
        std::shared_lock<std::shared_mutex> lock(componentsMutex_);
        for (const auto& [name, info] : components_) {
            if (info) {
                oss << name << ": " << componentStateToString(info->state)
                    << " (restarts: " << info->restartAttempts << ")" << std::endl;
            }
        }
    }

    // Circuit breaker info
    oss << std::endl << "--- Circuit Breakers ---" << std::endl;
    {
        std::shared_lock<std::shared_mutex> lock(circuitBreakersMutex_);
        for (const auto& [name, info] : circuitBreakers_) {
            oss << name << ": " << circuitBreakerStateToString(info.state)
                << " (failures: " << info.failureCount << ")" << std::endl;
        }
    }

    // Statistics
    oss << std::endl << "--- Statistics ---" << std::endl;
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        oss << "Connection failures: " << stats_.connectionFailures << std::endl;
        oss << "Memory allocation failures: " << stats_.memoryAllocationFailures << std::endl;
        oss << "Component crashes: " << stats_.componentCrashes << std::endl;
        oss << "Component restarts: " << stats_.componentRestarts << std::endl;
        oss << "Circuit breaker trips: " << stats_.circuitBreakerTrips << std::endl;
        oss << "Unrecoverable errors: " << stats_.unrecoverableErrors << std::endl;
    }

    oss << std::endl << "=== End Diagnostic Dump ===" << std::endl;

    return oss.str();
}

} // namespace core
} // namespace openrtmp
