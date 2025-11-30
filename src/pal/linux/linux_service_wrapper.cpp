// OpenRTMP - Cross-platform RTMP Server
// Linux Service Wrapper Implementation
//
// Requirements Covered: 7.4, 7.5
// - 7.4: Support binding to specific network interfaces
// - 7.5: Support running as a background service or daemon

#if defined(__linux__)

#include "openrtmp/pal/linux/linux_service_wrapper.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace openrtmp {
namespace pal {
namespace linux_pal {

// =============================================================================
// Static Members
// =============================================================================

LinuxServiceWrapper* LinuxServiceWrapper::instance_ = nullptr;
std::mutex LinuxServiceWrapper::instanceMutex_;

// =============================================================================
// Constructor / Destructor
// =============================================================================

LinuxServiceWrapper::LinuxServiceWrapper()
    : shutdownRequested_(false)
    , shutdownReason_(ShutdownReason::Unknown)
    , gracePeriod_(std::chrono::seconds(30))
    , signalHandlersInstalled_(false)
{
    std::lock_guard<std::mutex> lock(instanceMutex_);
    if (instance_ == nullptr) {
        instance_ = this;
    }
}

LinuxServiceWrapper::~LinuxServiceWrapper() {
    uninstallSignalHandlers();

    // Remove PID file if we created one
    if (!currentPidFile_.empty()) {
        removePidFile(currentPidFile_);
    }

    std::lock_guard<std::mutex> lock(instanceMutex_);
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

// =============================================================================
// Signal Handler Implementation
// =============================================================================

void LinuxServiceWrapper::signalHandler(int signum) {
    std::lock_guard<std::mutex> lock(instanceMutex_);
    if (instance_ != nullptr) {
        switch (signum) {
            case SIGTERM:
                instance_->shutdownReason_.store(ShutdownReason::ServiceStop);
                instance_->shutdownRequested_.store(true);
                break;

            case SIGINT:
                instance_->shutdownReason_.store(ShutdownReason::UserRequested);
                instance_->shutdownRequested_.store(true);
                break;

            case SIGHUP:
                // SIGHUP is used for config reload
                // Call reload callback outside of signal context if possible
                // For signal safety, just set a flag here
                break;

            case SIGUSR1:
            case SIGUSR2:
                // User-defined signals
                break;

            default:
                instance_->shutdownReason_.store(ShutdownReason::SignalReceived);
                instance_->shutdownRequested_.store(true);
                break;
        }
    }
}

core::Result<void, ServiceError> LinuxServiceWrapper::installSignalHandlers() {
    if (signalHandlersInstalled_) {
        return core::Result<void, ServiceError>::success();
    }

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &LinuxServiceWrapper::signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    // Install handler for SIGTERM (systemctl stop)
    if (sigaction(SIGTERM, &sa, nullptr) < 0) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::SignalHandlerFailed,
                         "Failed to install SIGTERM handler: " + std::string(strerror(errno)),
                         errno}
        );
    }

    // Install handler for SIGINT (Ctrl+C)
    if (sigaction(SIGINT, &sa, nullptr) < 0) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::SignalHandlerFailed,
                         "Failed to install SIGINT handler: " + std::string(strerror(errno)),
                         errno}
        );
    }

    // Install handler for SIGHUP (reload config)
    if (sigaction(SIGHUP, &sa, nullptr) < 0) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::SignalHandlerFailed,
                         "Failed to install SIGHUP handler: " + std::string(strerror(errno)),
                         errno}
        );
    }

    // Install handler for SIGUSR1
    if (sigaction(SIGUSR1, &sa, nullptr) < 0) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::SignalHandlerFailed,
                         "Failed to install SIGUSR1 handler: " + std::string(strerror(errno)),
                         errno}
        );
    }

    // Install handler for SIGUSR2
    if (sigaction(SIGUSR2, &sa, nullptr) < 0) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::SignalHandlerFailed,
                         "Failed to install SIGUSR2 handler: " + std::string(strerror(errno)),
                         errno}
        );
    }

    signalHandlersInstalled_ = true;
    return core::Result<void, ServiceError>::success();
}

core::Result<void, ServiceError> LinuxServiceWrapper::uninstallSignalHandlers() {
    if (!signalHandlersInstalled_) {
        return core::Result<void, ServiceError>::success();
    }

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    sigaction(SIGUSR1, &sa, nullptr);
    sigaction(SIGUSR2, &sa, nullptr);

    signalHandlersInstalled_ = false;
    return core::Result<void, ServiceError>::success();
}

void LinuxServiceWrapper::setShutdownCallback(ShutdownCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    shutdownCallback_ = std::move(callback);
}

void LinuxServiceWrapper::setReloadCallback(ReloadCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    reloadCallback_ = std::move(callback);
}

void LinuxServiceWrapper::setUserSignalCallback(int signalNum, UserSignalCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (signalNum == 1) {
        usr1Callback_ = std::move(callback);
    } else if (signalNum == 2) {
        usr2Callback_ = std::move(callback);
    }
}

// =============================================================================
// Shutdown Management
// =============================================================================

bool LinuxServiceWrapper::isShutdownRequested() const {
    return shutdownRequested_.load();
}

void LinuxServiceWrapper::requestShutdown(ShutdownReason reason) {
    shutdownReason_.store(reason);
    shutdownRequested_.store(true);

    // Invoke callback
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (shutdownCallback_) {
        shutdownCallback_(reason);
    }
}

ShutdownReason LinuxServiceWrapper::getShutdownReason() const {
    return shutdownReason_.load();
}

bool LinuxServiceWrapper::waitForShutdown(std::chrono::milliseconds timeout) {
    auto endTime = std::chrono::steady_clock::now() + timeout;

    while (!shutdownRequested_.load()) {
        if (std::chrono::steady_clock::now() >= endTime) {
            return false;  // Timeout
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return true;
}

void LinuxServiceWrapper::setGracePeriod(std::chrono::seconds period) {
    gracePeriod_ = period;
}

std::chrono::seconds LinuxServiceWrapper::getGracePeriod() const {
    return gracePeriod_;
}

// =============================================================================
// Systemd Unit Configuration Generation
// =============================================================================

std::string LinuxServiceWrapper::escapeSystemdString(const std::string& str) const {
    std::string result;
    result.reserve(str.size() + str.size() / 10);

    for (char c : str) {
        if (c == '%') {
            result += "%%";
        } else if (c == '"') {
            result += "\\\"";
        } else if (c == '\\') {
            result += "\\\\";
        } else {
            result += c;
        }
    }

    return result;
}

std::string LinuxServiceWrapper::getServiceTypeString(SystemdServiceType type) const {
    switch (type) {
        case SystemdServiceType::Simple:   return "simple";
        case SystemdServiceType::Exec:     return "exec";
        case SystemdServiceType::Forking:  return "forking";
        case SystemdServiceType::Oneshot:  return "oneshot";
        case SystemdServiceType::Dbus:     return "dbus";
        case SystemdServiceType::Notify:   return "notify";
        case SystemdServiceType::Idle:     return "idle";
        default:                           return "simple";
    }
}

std::string LinuxServiceWrapper::getRestartPolicyString(SystemdRestartPolicy policy) const {
    switch (policy) {
        case SystemdRestartPolicy::No:          return "no";
        case SystemdRestartPolicy::OnSuccess:   return "on-success";
        case SystemdRestartPolicy::OnFailure:   return "on-failure";
        case SystemdRestartPolicy::OnAbnormal:  return "on-abnormal";
        case SystemdRestartPolicy::OnWatchdog:  return "on-watchdog";
        case SystemdRestartPolicy::OnAbort:     return "on-abort";
        case SystemdRestartPolicy::Always:      return "always";
        default:                                return "on-failure";
    }
}

core::Result<std::string, ServiceError> LinuxServiceWrapper::generateSystemdUnit(
    const SystemdServiceConfig& config)
{
    // Validate configuration
    if (config.serviceName.empty()) {
        return core::Result<std::string, ServiceError>::error(
            ServiceError{ServiceErrorCode::InvalidConfiguration,
                         "Service name cannot be empty", 0}
        );
    }

    if (config.executablePath.empty()) {
        return core::Result<std::string, ServiceError>::error(
            ServiceError{ServiceErrorCode::InvalidConfiguration,
                         "Executable path cannot be empty", 0}
        );
    }

    std::ostringstream unit;

    // [Unit] section
    unit << "[Unit]\n";
    if (!config.description.empty()) {
        unit << "Description=" << escapeSystemdString(config.description) << "\n";
    } else {
        unit << "Description=" << config.serviceName << " service\n";
    }

    if (!config.after.empty()) {
        unit << "After=";
        for (size_t i = 0; i < config.after.size(); ++i) {
            if (i > 0) unit << " ";
            unit << config.after[i];
        }
        unit << "\n";
    } else {
        unit << "After=network.target network-online.target\n";
    }

    if (!config.wants.empty()) {
        unit << "Wants=";
        for (size_t i = 0; i < config.wants.size(); ++i) {
            if (i > 0) unit << " ";
            unit << config.wants[i];
        }
        unit << "\n";
    }

    if (!config.requires.empty()) {
        unit << "Requires=";
        for (size_t i = 0; i < config.requires.size(); ++i) {
            if (i > 0) unit << " ";
            unit << config.requires[i];
        }
        unit << "\n";
    }

    if (!config.before.empty()) {
        unit << "Before=";
        for (size_t i = 0; i < config.before.size(); ++i) {
            if (i > 0) unit << " ";
            unit << config.before[i];
        }
        unit << "\n";
    }

    unit << "\n";

    // [Service] section
    unit << "[Service]\n";
    unit << "Type=" << getServiceTypeString(config.serviceType) << "\n";

    // Build ExecStart
    std::string execStart = config.executablePath;
    for (const auto& arg : config.arguments) {
        execStart += " " + arg;
    }
    // For template units, add %i for instance identifier if configFile is set
    if (config.isTemplateUnit && !config.configFile.empty()) {
        execStart += " --config " + config.configFile;
    }
    unit << "ExecStart=" << execStart << "\n";

    // Working directory
    if (!config.workingDirectory.empty()) {
        unit << "WorkingDirectory=" << config.workingDirectory << "\n";
    }

    // User and group
    if (!config.userName.empty()) {
        unit << "User=" << config.userName << "\n";
    }
    if (!config.groupName.empty()) {
        unit << "Group=" << config.groupName << "\n";
    }

    // Environment variables
    for (const auto& [key, value] : config.environmentVariables) {
        unit << "Environment=\"" << key << "=" << escapeSystemdString(value) << "\"\n";
    }

    // Environment file
    if (!config.environmentFile.empty()) {
        unit << "EnvironmentFile=" << config.environmentFile << "\n";
    }

    // PID file (for forking type or template units)
    if (!config.pidFile.empty()) {
        unit << "PIDFile=" << config.pidFile << "\n";
    }

    // Restart configuration
    unit << "Restart=" << getRestartPolicyString(config.restart) << "\n";
    unit << "RestartSec=" << config.restartSec << "\n";

    // Timeouts
    if (config.timeoutStartSec > 0) {
        unit << "TimeoutStartSec=" << config.timeoutStartSec << "\n";
    }
    if (config.timeoutStopSec > 0) {
        unit << "TimeoutStopSec=" << config.timeoutStopSec << "\n";
    }

    // Watchdog (for notify type)
    if (config.watchdogSec > 0 && config.serviceType == SystemdServiceType::Notify) {
        unit << "WatchdogSec=" << config.watchdogSec << "\n";
    }

    // Resource limits
    if (config.limitNoFile > 0) {
        unit << "LimitNOFILE=" << config.limitNoFile << "\n";
    }
    if (config.limitNProc > 0) {
        unit << "LimitNPROC=" << config.limitNProc << "\n";
    }

    // Security options
    if (config.privateTmp) {
        unit << "PrivateTmp=true\n";
    }
    if (config.protectSystem) {
        unit << "ProtectSystem=strict\n";
    }
    if (config.protectHome) {
        unit << "ProtectHome=true\n";
    }
    if (config.noNewPrivileges) {
        unit << "NoNewPrivileges=true\n";
    }

    // Standard output/error
    if (!config.standardOutput.empty()) {
        unit << "StandardOutput=" << config.standardOutput << "\n";
    }
    if (!config.standardError.empty()) {
        unit << "StandardError=" << config.standardError << "\n";
    }

    unit << "\n";

    // [Install] section
    unit << "[Install]\n";
    if (!config.wantedBy.empty()) {
        unit << "WantedBy=" << config.wantedBy << "\n";
    }
    if (!config.alias.empty()) {
        unit << "Alias=" << config.alias << "\n";
    }

    return core::Result<std::string, ServiceError>::success(unit.str());
}

core::Result<void, ServiceError> LinuxServiceWrapper::saveUnitToFile(
    const SystemdServiceConfig& config, const std::string& path)
{
    auto unitResult = generateSystemdUnit(config);
    if (unitResult.isError()) {
        return core::Result<void, ServiceError>::error(unitResult.error());
    }

    std::ofstream file(path);
    if (!file.is_open()) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::FileWriteFailed,
                         "Failed to open file for writing: " + path,
                         errno}
        );
    }

    file << unitResult.value();
    file.close();

    if (file.fail()) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::FileWriteFailed,
                         "Failed to write unit file: " + path,
                         errno}
        );
    }

    return core::Result<void, ServiceError>::success();
}

// =============================================================================
// Network Interface Enumeration
// =============================================================================

core::Result<std::vector<NetworkInterfaceInfo>, ServiceError>
    LinuxServiceWrapper::enumerateNetworkInterfaces()
{
    std::vector<NetworkInterfaceInfo> interfaces;
    std::map<std::string, NetworkInterfaceInfo*> interfaceMap;

    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) < 0) {
        return core::Result<std::vector<NetworkInterfaceInfo>, ServiceError>::error(
            ServiceError{ServiceErrorCode::NetworkError,
                         "Failed to enumerate network interfaces: " + std::string(strerror(errno)),
                         errno}
        );
    }

    // Collect interface information
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_name == nullptr) {
            continue;
        }

        std::string name(ifa->ifa_name);

        if (interfaceMap.find(name) == interfaceMap.end()) {
            interfaces.emplace_back();
            NetworkInterfaceInfo& info = interfaces.back();
            info.name = name;
            info.displayName = name;
            info.isUp = (ifa->ifa_flags & IFF_UP) != 0;
            info.isLoopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
            info.supportsMulticast = (ifa->ifa_flags & IFF_MULTICAST) != 0;
            interfaceMap[name] = &info;
        }

        NetworkInterfaceInfo* info = interfaceMap[name];

        // Collect IP addresses
        if (ifa->ifa_addr != nullptr) {
            if (ifa->ifa_addr->sa_family == AF_INET) {
                // IPv4
                char ipStr[INET_ADDRSTRLEN];
                struct sockaddr_in* addr = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
                if (inet_ntop(AF_INET, &addr->sin_addr, ipStr, sizeof(ipStr))) {
                    info->ipv4Addresses.push_back(std::string(ipStr));
                }
            } else if (ifa->ifa_addr->sa_family == AF_INET6) {
                // IPv6
                char ipStr[INET6_ADDRSTRLEN];
                struct sockaddr_in6* addr = reinterpret_cast<struct sockaddr_in6*>(ifa->ifa_addr);
                if (inet_ntop(AF_INET6, &addr->sin6_addr, ipStr, sizeof(ipStr))) {
                    info->ipv6Addresses.push_back(std::string(ipStr));
                }
            }
        }
    }

    freeifaddrs(ifaddr);

    return core::Result<std::vector<NetworkInterfaceInfo>, ServiceError>::success(
        std::move(interfaces));
}

core::Result<bool, ServiceError> LinuxServiceWrapper::canBindToInterface(
    const std::string& interfaceNameOrAddress)
{
    // Special case: 0.0.0.0 means bind to all interfaces
    if (interfaceNameOrAddress == "0.0.0.0" || interfaceNameOrAddress == "::") {
        return core::Result<bool, ServiceError>::success(true);
    }

    // Check if it's an IP address
    struct sockaddr_in sa4;
    struct sockaddr_in6 sa6;

    bool isIPv4 = inet_pton(AF_INET, interfaceNameOrAddress.c_str(), &sa4.sin_addr) == 1;
    bool isIPv6 = inet_pton(AF_INET6, interfaceNameOrAddress.c_str(), &sa6.sin6_addr) == 1;

    if (isIPv4 || isIPv6) {
        // Try to create a socket and bind to verify
        int domain = isIPv4 ? AF_INET : AF_INET6;
        int fd = socket(domain, SOCK_STREAM, 0);
        if (fd < 0) {
            return core::Result<bool, ServiceError>::success(false);
        }

        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        bool canBind = false;
        if (isIPv4) {
            sa4.sin_family = AF_INET;
            sa4.sin_port = 0;  // Let system choose port
            canBind = bind(fd, reinterpret_cast<struct sockaddr*>(&sa4), sizeof(sa4)) == 0;
        } else {
            sa6.sin6_family = AF_INET6;
            sa6.sin6_port = 0;
            canBind = bind(fd, reinterpret_cast<struct sockaddr*>(&sa6), sizeof(sa6)) == 0;
        }

        close(fd);
        return core::Result<bool, ServiceError>::success(canBind);
    }

    // It's an interface name - check if it exists and is up
    auto interfacesResult = enumerateNetworkInterfaces();
    if (interfacesResult.isError()) {
        return core::Result<bool, ServiceError>::error(interfacesResult.error());
    }

    for (const auto& iface : interfacesResult.value()) {
        if (iface.name == interfaceNameOrAddress) {
            return core::Result<bool, ServiceError>::success(iface.isUp);
        }
    }

    return core::Result<bool, ServiceError>::success(false);
}

// =============================================================================
// PID File Management
// =============================================================================

core::Result<void, ServiceError> LinuxServiceWrapper::createPidFile(const std::string& path) {
    std::ofstream file(path);
    if (!file.is_open()) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::PidFileError,
                         "Failed to create PID file: " + path,
                         errno}
        );
    }

    file << getpid();
    file.close();

    if (file.fail()) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::PidFileError,
                         "Failed to write PID file: " + path,
                         errno}
        );
    }

    currentPidFile_ = path;
    return core::Result<void, ServiceError>::success();
}

core::Result<void, ServiceError> LinuxServiceWrapper::removePidFile(const std::string& path) {
    if (unlink(path.c_str()) != 0 && errno != ENOENT) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::PidFileError,
                         "Failed to remove PID file: " + path,
                         errno}
        );
    }

    if (currentPidFile_ == path) {
        currentPidFile_.clear();
    }

    return core::Result<void, ServiceError>::success();
}

core::Result<bool, ServiceError> LinuxServiceWrapper::checkPidFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        // File doesn't exist - no process running
        return core::Result<bool, ServiceError>::success(false);
    }

    pid_t pid;
    file >> pid;

    if (file.fail() || pid <= 0) {
        return core::Result<bool, ServiceError>::success(false);
    }

    // Check if process exists
    if (kill(pid, 0) == 0) {
        // Process exists
        return core::Result<bool, ServiceError>::success(true);
    }

    if (errno == ESRCH) {
        // Process doesn't exist
        return core::Result<bool, ServiceError>::success(false);
    }

    // Permission denied or other error - process likely exists
    return core::Result<bool, ServiceError>::success(true);
}

// =============================================================================
// Service Status
// =============================================================================

core::Result<ServiceStatus, ServiceError> LinuxServiceWrapper::getServiceStatus() {
    if (shutdownRequested_.load()) {
        return core::Result<ServiceStatus, ServiceError>::success(ServiceStatus::Stopped);
    }

    return core::Result<ServiceStatus, ServiceError>::success(ServiceStatus::Running);
}

core::Result<ServiceInfo, ServiceError> LinuxServiceWrapper::getServiceInfo() {
    ServiceInfo info;
    info.status = shutdownRequested_.load() ? ServiceStatus::Stopped : ServiceStatus::Running;
    info.pid = getpid();
    info.restartCount = 0;

    return core::Result<ServiceInfo, ServiceError>::success(info);
}

core::Result<void, ServiceError> LinuxServiceWrapper::reportHealthStatus(
    HealthStatus status, const std::string& message)
{
    std::string statusStr;
    switch (status) {
        case HealthStatus::Healthy:
            statusStr = "STATUS=Healthy";
            break;
        case HealthStatus::Degraded:
            statusStr = "STATUS=Degraded";
            break;
        case HealthStatus::Unhealthy:
            statusStr = "STATUS=Unhealthy";
            break;
    }

    if (!message.empty()) {
        statusStr += ": " + message;
    }

    if (!sendNotify(statusStr)) {
        // Not running under systemd - that's OK
    }

    return core::Result<void, ServiceError>::success();
}

// =============================================================================
// Systemd Notification (sd_notify)
// =============================================================================

bool LinuxServiceWrapper::sendNotify(const std::string& message) {
    const char* socketPath = getenv("NOTIFY_SOCKET");
    if (socketPath == nullptr || socketPath[0] == '\0') {
        // Not running under systemd with Type=notify
        return true;  // Not an error, just no-op
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    // Handle abstract socket (starts with @)
    if (socketPath[0] == '@') {
        addr.sun_path[0] = '\0';
        strncpy(&addr.sun_path[1], socketPath + 1, sizeof(addr.sun_path) - 2);
    } else {
        strncpy(addr.sun_path, socketPath, sizeof(addr.sun_path) - 1);
    }

    ssize_t sent = sendto(fd, message.c_str(), message.length(), MSG_NOSIGNAL,
                          reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

    close(fd);

    return sent >= 0;
}

core::Result<void, ServiceError> LinuxServiceWrapper::notifyReady() {
    if (!sendNotify("READY=1")) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::SystemdNotifyError,
                         "Failed to send READY notification",
                         errno}
        );
    }
    return core::Result<void, ServiceError>::success();
}

core::Result<void, ServiceError> LinuxServiceWrapper::notifyStopping() {
    if (!sendNotify("STOPPING=1")) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::SystemdNotifyError,
                         "Failed to send STOPPING notification",
                         errno}
        );
    }
    return core::Result<void, ServiceError>::success();
}

core::Result<void, ServiceError> LinuxServiceWrapper::notifyWatchdog() {
    if (!sendNotify("WATCHDOG=1")) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::SystemdNotifyError,
                         "Failed to send WATCHDOG notification",
                         errno}
        );
    }
    return core::Result<void, ServiceError>::success();
}

core::Result<void, ServiceError> LinuxServiceWrapper::notifyStatus(const std::string& status) {
    if (!sendNotify("STATUS=" + status)) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::SystemdNotifyError,
                         "Failed to send STATUS notification",
                         errno}
        );
    }
    return core::Result<void, ServiceError>::success();
}

} // namespace linux_pal
} // namespace pal
} // namespace openrtmp

#endif // __linux__
