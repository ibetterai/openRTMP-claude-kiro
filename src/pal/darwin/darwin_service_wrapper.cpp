// OpenRTMP - Cross-platform RTMP Server
// Darwin (macOS) Service Wrapper Implementation
//
// Requirements Covered: 7.4, 7.5, 7.7

#if defined(__APPLE__)

#include "openrtmp/pal/darwin/darwin_service_wrapper.hpp"

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
#include <sys/types.h>
#include <thread>
#include <unistd.h>

namespace openrtmp {
namespace pal {
namespace darwin {

// =============================================================================
// Static Members
// =============================================================================

DarwinServiceWrapper* DarwinServiceWrapper::instance_ = nullptr;
std::mutex DarwinServiceWrapper::instanceMutex_;

// =============================================================================
// Constructor / Destructor
// =============================================================================

DarwinServiceWrapper::DarwinServiceWrapper()
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

DarwinServiceWrapper::~DarwinServiceWrapper() {
    uninstallSignalHandlers();

    std::lock_guard<std::mutex> lock(instanceMutex_);
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

// =============================================================================
// Signal Handler Implementation
// =============================================================================

void DarwinServiceWrapper::signalHandler(int signum) {
    std::lock_guard<std::mutex> lock(instanceMutex_);
    if (instance_ != nullptr) {
        ShutdownReason reason = ShutdownReason::SignalReceived;

        switch (signum) {
            case SIGTERM:
                reason = ShutdownReason::ServiceStop;
                break;
            case SIGINT:
                reason = ShutdownReason::UserRequested;
                break;
            case SIGHUP:
                // SIGHUP could be used for config reload
                // For now, treat as shutdown
                reason = ShutdownReason::SignalReceived;
                break;
            default:
                reason = ShutdownReason::SignalReceived;
                break;
        }

        instance_->shutdownReason_.store(reason);
        instance_->shutdownRequested_.store(true);

        // Invoke callback if set (note: must be signal-safe)
        // In practice, callbacks should be very simple in signal context
    }
}

core::Result<void, ServiceError> DarwinServiceWrapper::installSignalHandlers() {
    if (signalHandlersInstalled_) {
        return core::Result<void, ServiceError>::success();
    }

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &DarwinServiceWrapper::signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    // Install handler for SIGTERM
    if (sigaction(SIGTERM, &sa, nullptr) < 0) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::SignalHandlerFailed,
                         "Failed to install SIGTERM handler: " + std::string(strerror(errno))}
        );
    }

    // Install handler for SIGINT
    if (sigaction(SIGINT, &sa, nullptr) < 0) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::SignalHandlerFailed,
                         "Failed to install SIGINT handler: " + std::string(strerror(errno))}
        );
    }

    // Install handler for SIGHUP
    if (sigaction(SIGHUP, &sa, nullptr) < 0) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::SignalHandlerFailed,
                         "Failed to install SIGHUP handler: " + std::string(strerror(errno))}
        );
    }

    signalHandlersInstalled_ = true;
    return core::Result<void, ServiceError>::success();
}

core::Result<void, ServiceError> DarwinServiceWrapper::uninstallSignalHandlers() {
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

    signalHandlersInstalled_ = false;
    return core::Result<void, ServiceError>::success();
}

void DarwinServiceWrapper::setShutdownCallback(ShutdownCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    shutdownCallback_ = std::move(callback);
}

// =============================================================================
// Shutdown Management
// =============================================================================

bool DarwinServiceWrapper::isShutdownRequested() const {
    return shutdownRequested_.load();
}

void DarwinServiceWrapper::requestShutdown(ShutdownReason reason) {
    shutdownReason_.store(reason);
    shutdownRequested_.store(true);

    // Invoke callback
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (shutdownCallback_) {
        shutdownCallback_(reason);
    }
}

ShutdownReason DarwinServiceWrapper::getShutdownReason() const {
    return shutdownReason_.load();
}

bool DarwinServiceWrapper::waitForShutdown(std::chrono::milliseconds timeout) {
    auto endTime = std::chrono::steady_clock::now() + timeout;

    while (!shutdownRequested_.load()) {
        if (std::chrono::steady_clock::now() >= endTime) {
            return false;  // Timeout
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return true;
}

void DarwinServiceWrapper::setGracePeriod(std::chrono::seconds period) {
    gracePeriod_ = period;
}

std::chrono::seconds DarwinServiceWrapper::getGracePeriod() const {
    return gracePeriod_;
}

// =============================================================================
// Launchd Configuration Generation
// =============================================================================

std::string DarwinServiceWrapper::escapePlistString(const std::string& str) const {
    std::string result;
    result.reserve(str.size() + str.size() / 10);

    for (char c : str) {
        switch (c) {
            case '&':
                result += "&amp;";
                break;
            case '<':
                result += "&lt;";
                break;
            case '>':
                result += "&gt;";
                break;
            case '"':
                result += "&quot;";
                break;
            case '\'':
                result += "&apos;";
                break;
            default:
                result += c;
                break;
        }
    }

    return result;
}

bool DarwinServiceWrapper::isValidPlistIdentifier(const std::string& identifier) const {
    if (identifier.empty()) {
        return false;
    }

    // Should follow reverse-DNS naming convention
    // Basic validation: contains at least one dot and starts with alphanumeric
    if (identifier.find('.') == std::string::npos) {
        return false;
    }

    if (!std::isalnum(identifier[0])) {
        return false;
    }

    // Only allow alphanumeric, dots, hyphens, underscores
    for (char c : identifier) {
        if (!std::isalnum(c) && c != '.' && c != '-' && c != '_') {
            return false;
        }
    }

    return true;
}

core::Result<std::string, ServiceError> DarwinServiceWrapper::generateLaunchdPlist(
    const ServiceConfig& config)
{
    // Validate configuration
    if (config.serviceName.empty()) {
        return core::Result<std::string, ServiceError>::error(
            ServiceError{ServiceErrorCode::InvalidConfiguration,
                         "Service name cannot be empty"}
        );
    }

    if (config.executablePath.empty()) {
        return core::Result<std::string, ServiceError>::error(
            ServiceError{ServiceErrorCode::InvalidConfiguration,
                         "Executable path cannot be empty"}
        );
    }

    std::ostringstream plist;

    // XML header
    plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    plist << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    plist << "<plist version=\"1.0\">\n";
    plist << "<dict>\n";

    // Label (required)
    plist << "\t<key>Label</key>\n";
    plist << "\t<string>" << escapePlistString(config.serviceName) << "</string>\n";

    // ProgramArguments (required)
    plist << "\t<key>ProgramArguments</key>\n";
    plist << "\t<array>\n";
    plist << "\t\t<string>" << escapePlistString(config.executablePath) << "</string>\n";
    for (const auto& arg : config.arguments) {
        plist << "\t\t<string>" << escapePlistString(arg) << "</string>\n";
    }
    plist << "\t</array>\n";

    // WorkingDirectory
    if (!config.workingDirectory.empty()) {
        plist << "\t<key>WorkingDirectory</key>\n";
        plist << "\t<string>" << escapePlistString(config.workingDirectory) << "</string>\n";
    }

    // RunAtLoad
    plist << "\t<key>RunAtLoad</key>\n";
    plist << "\t<" << (config.runAtLoad ? "true" : "false") << "/>\n";

    // KeepAlive
    if (config.keepAlive) {
        plist << "\t<key>KeepAlive</key>\n";
        plist << "\t<true/>\n";
    }

    // AbandonProcessGroup
    if (config.abandonProcessGroup) {
        plist << "\t<key>AbandonProcessGroup</key>\n";
        plist << "\t<true/>\n";
    }

    // ThrottleInterval
    if (config.throttleInterval > 0) {
        plist << "\t<key>ThrottleInterval</key>\n";
        plist << "\t<integer>" << config.throttleInterval << "</integer>\n";
    }

    // StandardOutPath
    if (!config.standardOutPath.empty()) {
        plist << "\t<key>StandardOutPath</key>\n";
        plist << "\t<string>" << escapePlistString(config.standardOutPath) << "</string>\n";
    }

    // StandardErrorPath
    if (!config.standardErrorPath.empty()) {
        plist << "\t<key>StandardErrorPath</key>\n";
        plist << "\t<string>" << escapePlistString(config.standardErrorPath) << "</string>\n";
    }

    // EnvironmentVariables
    if (!config.environmentVariables.empty()) {
        plist << "\t<key>EnvironmentVariables</key>\n";
        plist << "\t<dict>\n";
        for (const auto& [key, value] : config.environmentVariables) {
            plist << "\t\t<key>" << escapePlistString(key) << "</key>\n";
            plist << "\t\t<string>" << escapePlistString(value) << "</string>\n";
        }
        plist << "\t</dict>\n";
    }

    // UserName
    if (!config.userName.empty()) {
        plist << "\t<key>UserName</key>\n";
        plist << "\t<string>" << escapePlistString(config.userName) << "</string>\n";
    }

    // GroupName
    if (!config.groupName.empty()) {
        plist << "\t<key>GroupName</key>\n";
        plist << "\t<string>" << escapePlistString(config.groupName) << "</string>\n";
    }

    plist << "</dict>\n";
    plist << "</plist>\n";

    return core::Result<std::string, ServiceError>::success(plist.str());
}

core::Result<void, ServiceError> DarwinServiceWrapper::savePlistToFile(
    const ServiceConfig& config, const std::string& path)
{
    auto plistResult = generateLaunchdPlist(config);
    if (plistResult.isError()) {
        return core::Result<void, ServiceError>::error(plistResult.error());
    }

    std::ofstream file(path);
    if (!file.is_open()) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::FileWriteFailed,
                         "Failed to open file for writing: " + path}
        );
    }

    file << plistResult.value();
    file.close();

    if (file.fail()) {
        return core::Result<void, ServiceError>::error(
            ServiceError{ServiceErrorCode::FileWriteFailed,
                         "Failed to write plist to file: " + path}
        );
    }

    return core::Result<void, ServiceError>::success();
}

// =============================================================================
// Network Interface Enumeration
// =============================================================================

core::Result<std::vector<NetworkInterfaceInfo>, ServiceError>
    DarwinServiceWrapper::enumerateNetworkInterfaces()
{
    std::vector<NetworkInterfaceInfo> interfaces;
    std::map<std::string, NetworkInterfaceInfo*> interfaceMap;

    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) < 0) {
        return core::Result<std::vector<NetworkInterfaceInfo>, ServiceError>::error(
            ServiceError{ServiceErrorCode::NetworkError,
                         "Failed to enumerate network interfaces: " + std::string(strerror(errno))}
        );
    }

    // First pass: collect interface names and flags
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

core::Result<bool, ServiceError> DarwinServiceWrapper::canBindToInterface(
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

    // It's an interface name - check if it exists
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
// Network Permissions
// =============================================================================

core::Result<NetworkPermissionStatus, ServiceError>
    DarwinServiceWrapper::checkNetworkPermissionStatus()
{
    // On macOS, local network permission is typically granted for server applications
    // The permission dialog is triggered when the app first binds to a network interface
    // Here we return a reasonable default - in a full implementation, this would use
    // the private TCC (Transparency, Consent, Control) framework
    //
    // For command-line tools and daemons running as root or with network entitlements,
    // permission is generally granted by default.

    // Try to create a test socket to see if we have network access
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (errno == EPERM || errno == EACCES) {
            return core::Result<NetworkPermissionStatus, ServiceError>::success(
                NetworkPermissionStatus::Denied);
        }
        return core::Result<NetworkPermissionStatus, ServiceError>::success(
            NetworkPermissionStatus::NotDetermined);
    }

    close(fd);
    return core::Result<NetworkPermissionStatus, ServiceError>::success(
        NetworkPermissionStatus::Granted);
}

void DarwinServiceWrapper::requestNetworkPermission(NetworkPermissionCallback callback) {
    // On macOS, local network permission dialog is shown automatically when
    // the application first attempts to use the local network. For command-line
    // tools and services, this is typically handled by the system.
    //
    // In a GUI application, we would use NSLocalNetworkPermissionPrompter
    // (private API) or simply trigger network activity to show the dialog.

    // For now, check current status and invoke callback
    auto statusResult = checkNetworkPermissionStatus();
    if (statusResult.isSuccess() && callback) {
        callback(statusResult.value());
    }
}

// =============================================================================
// Service Status
// =============================================================================

core::Result<ServiceStatus, ServiceError> DarwinServiceWrapper::getServiceStatus() {
    // For a running service, we can check our own status
    if (shutdownRequested_.load()) {
        return core::Result<ServiceStatus, ServiceError>::success(ServiceStatus::Stopped);
    }

    return core::Result<ServiceStatus, ServiceError>::success(ServiceStatus::Running);
}

core::Result<ServiceInfo, ServiceError> DarwinServiceWrapper::getServiceInfo() {
    ServiceInfo info;
    info.status = shutdownRequested_.load() ? ServiceStatus::Stopped : ServiceStatus::Running;
    info.pid = getpid();
    info.restartCount = 0;

    return core::Result<ServiceInfo, ServiceError>::success(info);
}

} // namespace darwin
} // namespace pal
} // namespace openrtmp

#endif // __APPLE__
